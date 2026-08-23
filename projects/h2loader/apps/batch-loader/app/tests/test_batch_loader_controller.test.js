import { describe, expect, it, vi } from "vitest";
import { BatchLoaderController } from "../src/batch_loader_controller";

const items = (count) => Array.from({ length: count }, (_, index) => ({ portId: `port-${index}` }));
const deferred = () => { let resolve; const promise = new Promise((r) => { resolve = r; }); return { promise, resolve }; };

describe("BatchLoaderController", () => {
  it("runs at most four jobs and isolates failures", async () => {
    let active = 0;
    let peak = 0;
    const updates = [];
    const controller = new BatchLoaderController({ onUpdate: (...args) => updates.push(args) });
    await controller.run(items(7), async (item) => {
      active += 1;
      peak = Math.max(peak, active);
      await Promise.resolve();
      active -= 1;
      if (item.portId === "port-2") throw new Error("disconnected");
      return { version: "next" };
    });
    expect(peak).toBe(4);
    expect(updates.some(([id, state]) => id === "port-2" && state.state === "error")).toBe(true);
    expect(updates.some(([id, state]) => id === "port-6" && state.state === "success")).toBe(true);
  });

  it("ignores a cancelled device's late completion", async () => {
    const updates = [];
    const pending = deferred();
    const controller = new BatchLoaderController({ onUpdate: (...args) => updates.push(args) });
    const run = controller.run(items(1), async () => pending.promise);
    controller.cancel();
    pending.resolve({ version: "stale" });
    await run;
    expect(updates.some(([, state]) => state.version === "stale")).toBe(false);
  });

  it("forwards progress for the running generation", async () => {
    const onUpdate = vi.fn();
    const controller = new BatchLoaderController({ onUpdate });
    await controller.run(items(1), async (_item, { onProgress }) => {
      onProgress({ progress: 42, phase: "staging" });
      return {};
    });
    expect(onUpdate).toHaveBeenCalledWith("port-0", expect.objectContaining({ progress: 42 }), expect.any(Number));
  });

  it("keeps operations that invalidate identity visibly stale", async () => {
    const onUpdate = vi.fn();
    const controller = new BatchLoaderController({ onUpdate });
    await controller.run(items(1), async () => ({ stale: true }));
    expect(onUpdate).toHaveBeenCalledWith(
      "port-0",
      expect.objectContaining({ state: "stale", progress: 100 }),
      expect.any(Number),
    );
  });

  it("runs independent devices concurrently instead of cancelling each other", async () => {
    const controller = new BatchLoaderController({ onUpdate: () => {} });
    const first = deferred();
    const second = deferred();
    const runA = controller.run([{ portId: "a" }], async () => first.promise);
    const runB = controller.run([{ portId: "b" }], async () => second.promise);
    // Both devices are busy at the same time; neither cancelled the other.
    expect(controller.isBusy("a")).toBe(true);
    expect(controller.isBusy("b")).toBe(true);
    first.resolve({});
    second.resolve({});
    expect(await runA).toBe(true);
    expect(await runB).toBe(true);
  });

  it("skips a device that is already busy", async () => {
    const controller = new BatchLoaderController({ onUpdate: () => {} });
    const pending = deferred();
    const execute = vi.fn(async () => pending.promise);
    const first = controller.run([{ portId: "a" }], execute);
    await controller.run([{ portId: "a" }], execute); // ignored while busy
    expect(execute).toHaveBeenCalledTimes(1);
    pending.resolve({});
    await first;
  });

  it("cancels one device without disturbing the others", async () => {
    const updates = [];
    const controller = new BatchLoaderController({ onUpdate: (...args) => updates.push(args) });
    const a = deferred();
    const b = deferred();
    const runA = controller.run([{ portId: "a" }], async () => a.promise);
    const runB = controller.run([{ portId: "b" }], async () => b.promise);
    controller.cancelDevice("a");
    // "a" stays busy until its runner exits; "b" is untouched.
    expect(controller.isBusy("a")).toBe(true);
    expect(controller.isBusy("b")).toBe(true);
    a.resolve({ version: "late" });
    b.resolve({ version: "kept" });
    await runA;
    await runB;
    expect(controller.isBusy("a")).toBe(false);
    expect(updates.some(([id, state]) => id === "a" && state.version === "late")).toBe(false);
    expect(updates.some(([id, state]) => id === "b" && state.version === "kept")).toBe(true);
  });

  it("cancels every running device at once", async () => {
    const controller = new BatchLoaderController({ onUpdate: () => {} });
    const pendings = items(3).map(() => deferred());
    const run = controller.run(items(3), async (item) => pendings[Number(item.portId.split("-")[1])].promise);
    expect(controller.busyCount).toBe(3);
    controller.cancel();
    // Cancelled runners keep their device busy until each one exits.
    expect(controller.busyCount).toBe(3);
    pendings.forEach((p) => p.resolve({}));
    await run;
    expect(controller.busyCount).toBe(0);
  });
});

describe("BatchLoaderController concurrency override", () => {
  it("lets scans cover every device while writes stay capped at four", async () => {
    const controller = new BatchLoaderController({ onUpdate: () => {} });
    let active = 0;
    let peak = 0;
    const hold = [];
    const run = controller.run(items(7), async () => {
      active += 1;
      peak = Math.max(peak, active);
      await new Promise((resolve) => hold.push(() => { active -= 1; resolve({}); }));
    }, { concurrency: 7 });
    await Promise.resolve();
    expect(peak).toBe(7);
    hold.forEach((release) => release());
    await run;
  });
});

describe("BatchLoaderController slot accounting", () => {
  it("does not admit an extra write when a queued device is cancelled", async () => {
    const controller = new BatchLoaderController({ onUpdate: () => {} });
    let active = 0;
    let peak = 0;
    // Jobs stay busy until the whole run is cancelled.
    const run = controller.run(items(6), async (_item, { signal }) => {
      active += 1;
      peak = Math.max(peak, active);
      await new Promise((resolve) => signal.addEventListener("abort", resolve, { once: true }));
      active -= 1;
      return {};
    });
    await Promise.resolve();
    expect(peak).toBe(4);
    // port-5 is still queued: cancelling it must not free a running slot.
    controller.cancelDevice("port-5");
    await Promise.resolve();
    await Promise.resolve();
    expect(peak).toBe(4);
    controller.cancel();
    await run;
  });

  it("keeps a cancelled running job counted until it exits", async () => {
    const controller = new BatchLoaderController({ onUpdate: () => {} });
    let live = 0;
    let peak = 0;
    const settle = new Map();
    const run = controller.run(items(6), async (item) => {
      live += 1;
      peak = Math.max(peak, live);
      await new Promise((resolve) => settle.set(item.portId, resolve));
      live -= 1;
      return {};
    });
    await Promise.resolve();
    expect(peak).toBe(4);
    // Cancel a running device: its transport is still unwinding, so the slot
    // must not be handed to a queued job yet.
    controller.cancelDevice("port-0");
    await Promise.resolve();
    await Promise.resolve();
    expect(peak).toBe(4);
    // Once the cancelled runner actually exits, the queue may advance.
    settle.get("port-0")();
    await Promise.resolve();
    await Promise.resolve();
    expect(peak).toBe(4);
    controller.cancel();
    settle.forEach((resolve) => resolve());
    await run;
  });

  it("refuses new work on a cancelled port until its runner exits", async () => {
    const controller = new BatchLoaderController({ onUpdate: () => {} });
    const settle = new Map();
    const execute = vi.fn(async (item) =>
      new Promise((resolve) => settle.set(item.portId, () => resolve({}))));
    const first = controller.run([{ portId: "a" }, { portId: "b" }], execute);
    await Promise.resolve();
    controller.cancelDevice("a");
    // Free an unrelated slot; port "a" is still unwinding and must stay busy.
    settle.get("b")();
    await Promise.resolve();
    await Promise.resolve();
    expect(controller.isBusy("a")).toBe(true);
    const rejected = controller.run([{ portId: "a" }], execute);
    await Promise.resolve();
    expect(execute).toHaveBeenCalledTimes(2); // no third session on "a"
    await expect(rejected).resolves.toBe(true);
    settle.get("a")();
    await first;
    expect(controller.isBusy("a")).toBe(false);
  });

  it("settles the caller when a queued device is cancelled", async () => {
    const controller = new BatchLoaderController({ onUpdate: () => {} });
    const run = controller.run(items(6), async (_item, { signal }) =>
      new Promise((resolve) => signal.addEventListener("abort", () => resolve({}), { once: true })));
    await Promise.resolve();
    controller.cancel();
    // Queued jobs must resolve too, or the caller would await forever.
    await expect(run).resolves.toBe(false);
  });
});
