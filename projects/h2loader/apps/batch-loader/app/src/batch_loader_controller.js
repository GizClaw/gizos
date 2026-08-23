// Per-device scheduler.
//
// A serial port can only serve one operation at a time, so the only hard rule
// is that a device is busy or it is not. Operations started from different
// entry points (batch header, single-row menu) share one concurrency budget
// and otherwise run independently: cancelling or failing one device never
// disturbs another.
//
// Each device carries its own generation counter, so a late completion from a
// cancelled run is ignored without affecting the device's current run.

export class BatchLoaderController {
  constructor({ maxConcurrent = 4, onUpdate = () => {} } = {}) {
    this.maxConcurrent = maxConcurrent;
    this.onUpdate = onUpdate;
    this.devices = new Map(); // portId -> { generation, abortController, active }
    this.queue = [];
    this.activeCount = 0;
  }

  entry(portId) {
    let record = this.devices.get(portId);
    if (!record) {
      // active: the device owns a row-level operation (queued or running).
      // running: the operation actually acquired a concurrency slot.
      record = { generation: 0, abortController: null, active: false, running: false };
      this.devices.set(portId, record);
    }
    return record;
  }

  /**
   * A device stays busy until its runner exits, so a cancelled port cannot
   * accept new work while its serial session is still unwinding.
   */
  isBusy(portId) {
    const record = this.entry(portId);
    return record.active || record.running;
  }

  busyIds() {
    const ids = [];
    for (const [portId, record] of this.devices) {
      if (record.active || record.running) ids.push(portId);
    }
    return ids;
  }

  get busyCount() {
    return this.busyIds().length;
  }

  /** Cancel one device's work; other devices keep running. */
  cancelDevice(portId) {
    const record = this.entry(portId);
    record.generation += 1;
    record.abortController?.abort();
    record.abortController = null;
    // Drop anything still queued for this device, settling its waiter so the
    // caller's run() does not hang on work that will never start.
    this.queue = this.queue.filter((item) => {
      if (item.portId !== portId) return true;
      item.resolve(false);
      return false;
    });
    // A running job keeps its slot until its own runner exits: releasing it
    // here would release the same slot twice (again in #runJob's finally) and
    // admit an extra write while the cancelled transport is still unwinding.
    record.active = false;
    this.#pump();
  }

  /** Cancel every device currently running or queued. */
  cancel() {
    for (const portId of [...this.devices.keys()]) {
      const record = this.entry(portId);
      record.generation += 1;
      record.abortController?.abort();
      record.abortController = null;
      record.active = false;
    }
    // Queued work never took a slot, so settle it here; every running job
    // keeps its slot until its own runner releases it.
    this.queue.forEach((item) => item.resolve(false));
    this.queue = [];
  }

  /**
   * Run `execute` for each item. Items already busy are skipped so a second
   * request can never open the same port twice. Resolves when this call's
   * items have finished (or been cancelled).
   *
   * `concurrency` overrides the shared cap for this call: non-destructive
   * scans cover the whole authorized list, while the four-device limit applies
   * to operations that write to a device.
   */
  async run(items, execute, { concurrency } = {}) {
    const accepted = items.filter((item) => !this.isBusy(item.portId));
    if (!accepted.length) return true;

    accepted.forEach((item) => {
      const record = this.entry(item.portId);
      record.active = true;
      this.onUpdate(item.portId, { state: "queued", progress: 0 }, record.generation);
    });

    const limit = concurrency === undefined ? this.maxConcurrent : concurrency;
    const results = accepted.map((item) => new Promise((resolve) => {
      this.queue.push({ portId: item.portId, item, execute, resolve, limit });
    }));
    this.#pump();
    const settled = await Promise.all(results);
    return settled.every(Boolean);
  }

  #pump() {
    while (this.queue.length && this.activeCount < this.queue[0].limit) {
      const job = this.queue.shift();
      this.activeCount += 1;
      this.entry(job.portId).running = true;
      void this.#runJob(job);
    }
  }

  async #runJob({ portId, item, execute, resolve }) {
    const record = this.entry(portId);
    const generation = record.generation;
    const abortController = new AbortController();
    record.abortController = abortController;
    const signal = abortController.signal;
    const current = () => record.generation === generation && !signal.aborted;

    this.onUpdate(portId, { state: "busy", progress: 0 }, generation);
    let ok = false;
    try {
      const result = await execute(item, {
        signal,
        onProgress: (progress) => {
          if (current()) this.onUpdate(portId, { ...progress, state: "busy" }, generation);
        },
      });
      if (current()) {
        this.onUpdate(portId, {
          ...result,
          state: result?.stale ? "stale" : "success",
          progress: 100,
        }, generation);
        ok = true;
      }
    } catch (error) {
      if (current()) {
        this.onUpdate(portId, {
          state: signal.aborted || error.name === "AbortError" ? "cancelled" : "error",
          error: error.message,
        }, generation);
      }
    } finally {
      if (record.generation === generation) {
        record.active = false;
        record.abortController = null;
      }
      // This runner acquired the slot in #pump, so it is the only place that
      // releases it -- including when the job was cancelled mid-flight.
      record.running = false;
      this.activeCount = Math.max(0, this.activeCount - 1);
      resolve(ok);
      this.#pump();
    }
  }
}
