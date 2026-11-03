// Ported from GizClaw/gizway www/src/sdk/h2loader-controller.ts at 547d24c.
// The browser owns one bounded queue for all selected device operations.
import { createTransferTracker } from './progress.js';

export class H2LoaderController {
  constructor(onUpdate, maxConcurrent = 16) {
    this.onUpdate = onUpdate;
    this.maxConcurrent = maxConcurrent;
    this.activeCount = 0;
    this.devices = new Map();
    this.queue = [];
  }

  entry(portId) {
    let record = this.devices.get(portId);
    if (!record) {
      record = { active: false, generation: 0, running: false };
      this.devices.set(portId, record);
    }
    return record;
  }

  isBusy(portId) {
    const record = this.entry(portId);
    return record.active || record.running;
  }

  busyIds() {
    return [...this.devices.entries()]
      .filter(([, record]) => record.active || record.running)
      .map(([portId]) => portId);
  }

  cancelDevice(portId) {
    const record = this.entry(portId);
    record.generation += 1;
    record.abortController?.abort();
    record.abortController = undefined;
    record.active = false;
    for (let index = this.queue.length - 1; index >= 0; index -= 1) {
      const job = this.queue[index];
      if (job.portId !== portId) continue;
      this.queue.splice(index, 1);
      job.resolve(false);
    }
    this.pump();
  }

  cancel() {
    for (const record of this.devices.values()) {
      record.generation += 1;
      record.abortController?.abort();
      record.abortController = undefined;
      record.active = false;
    }
    this.queue.splice(0).forEach((job) => job.resolve(false));
  }

  async run(items, execute, options = {}) {
    const accepted = items.filter((item) => !this.isBusy(item.portId));
    if (!accepted.length) return true;
    accepted.forEach((item) => {
      this.entry(item.portId).active = true;
      this.onUpdate(item.portId, { progress: 0, phase: null, error: null, detail: null, transfer: null, state: 'queued' });
    });
    const limit = options.concurrency ?? this.maxConcurrent;
    const results = accepted.map((item) => new Promise((resolve) => {
      this.queue.push({ execute, item, limit, portId: item.portId, resolve });
    }));
    this.pump();
    return (await Promise.all(results)).every(Boolean);
  }

  pump() {
    while (this.queue.length && this.activeCount < this.queue[0].limit) {
      const job = this.queue.shift();
      if (!job) break;
      this.activeCount += 1;
      this.entry(job.portId).running = true;
      void this.runJob(job);
    }
  }

  async runJob(job) {
    const record = this.entry(job.portId);
    const generation = record.generation;
    const abortController = new AbortController();
    record.abortController = abortController;
    const current = () => record.generation === generation && !abortController.signal.aborted;
    const trackTransfer = createTransferTracker();
    let ok = false;
    this.onUpdate(job.portId, { progress: 0, state: 'busy' });
    try {
      const result = await job.execute(job.item, {
        signal: abortController.signal,
        onProgress: (progress) => {
          if (current()) this.onUpdate(job.portId, { ...progress, transfer: trackTransfer(progress), state: 'busy' });
        },
      });
      if (current()) {
        this.onUpdate(job.portId, {
          ...result,
          progress: 100,
          state: result.stale ? 'stale' : 'success',
        });
        ok = true;
      }
    } catch (error) {
      if (current()) {
        const aborted = abortController.signal.aborted ||
          (error instanceof DOMException && error.name === 'AbortError');
        this.onUpdate(job.portId, {
          error: error instanceof Error ? error.message : String(error),
          state: aborted ? 'cancelled' : 'error',
        });
      }
    } finally {
      if (record.generation === generation) {
        record.abortController = undefined;
        record.active = false;
      }
      record.running = false;
      this.activeCount = Math.max(0, this.activeCount - 1);
      job.resolve(ok);
      this.pump();
    }
  }
}
