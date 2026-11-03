import { afterEach, expect, test } from "bun:test";
import { mkdtemp, mkdir, readFile, realpath, writeFile, rm, readdir, rename } from "node:fs/promises";
import { tmpdir, hostname } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { exists, locked } from "../src/common";
import { configuration, configureRoot } from "../src/manager";

const temporary: string[] = [];
afterEach(async () => {
  await Promise.all(temporary.splice(0).map(path => rm(path, { recursive: true, force: true })));
});
async function directory() {
  const path = await realpath(await mkdtemp(join(process.env.TEST_TMPDIR ?? tmpdir(), "gizos-lock-")));
  temporary.push(path);
  return path;
}

test("recovers a lock left by an exited process", async () => {
  const root = await directory();
  const child = Bun.spawn([process.execPath, "-e", "process.exit(0)"]);
  await child.exited;
  const lock = join(root, "locks", "stale");
  await mkdir(lock, { recursive: true });
  await writeFile(join(lock, "owner.json"), JSON.stringify({ pid: child.pid, host: hostname() }));
  expect(await locked(root, "stale", async () => 42)).toBe(42);
});
test("concurrent tasks and processes recover an abandoned lock without overlapping callbacks", async () => {
  const root = await directory();
  const dead = Bun.spawn([process.execPath, "-e", "process.exit(0)"]);
  await dead.exited;
  const lock = join(root, "locks", "stale");
  await mkdir(lock, { recursive: true });
  await writeFile(join(lock, "owner.json"), JSON.stringify({ pid: dead.pid, host: hostname() }));
  let running = 0;
  let peak = 0;
  const tasks = await Promise.allSettled(Array.from({ length: 8 }, () => locked(root, "stale", async () => {
    peak = Math.max(peak, ++running);
    try { await Bun.sleep(30); }
    finally { running--; }
  })));
  expect(tasks.every(result => result.status === "fulfilled")).toBe(true);
  expect(peak).toBe(1);
  await mkdir(lock, { recursive: true });
  await writeFile(join(lock, "owner.json"), JSON.stringify({ pid: dead.pid, host: hostname() }));
  const common = pathToFileURL(join(import.meta.dir, "../src/common.ts")).href;
  const children = Array.from({ length: 8 }, (_, index) => Bun.spawn([process.execPath, "-e", `
import { locked, exists } from ${JSON.stringify(common)};
import { mkdir, writeFile, rm } from "node:fs/promises";
import { join } from "node:path";
const root = ${JSON.stringify(root)};
await writeFile(join(root, "ready-${index}"), "");
while (!await exists(join(root, "go"))) await Bun.sleep(5);
await locked(root, "stale", async () => {
  const active = join(root, "active");
  await mkdir(active);
  try {
    await Bun.sleep(40);
    await writeFile(join(root, "completed-${index}"), "");
  } finally { await rm(active, { recursive: true }); }
});
`], { stdout: "ignore", stderr: "pipe" }));
  try {
    const deadline = Date.now() + 10_000;
    while ((await readdir(root)).filter(name => name.startsWith("ready-")).length !== children.length) {
      if (Date.now() > deadline) throw new Error("Lock contenders did not start");
      await Bun.sleep(10);
    }
    await writeFile(join(root, "go"), "");
    const results = await Promise.all(children.map(async child => ({
      code: await child.exited, error: await new Response(child.stderr).text(),
    })));
    expect(results).toEqual(children.map(() => ({ code: 0, error: "" })));
    expect((await readdir(root)).filter(name => name.startsWith("completed-"))).toHaveLength(children.length);
    expect(await readdir(join(root, "locks"))).toEqual([]);
  } finally {
    for (const child of children) if (child.exitCode === null) child.kill();
    await Promise.all(children.map(child => child.exited));
  }
});
test("lock release preserves a replacement directory", async () => {
  const root = await directory();
  const lock = join(root, "locks", "replaced");
  await locked(root, "replaced", async () => {
    await rename(lock, `${lock}-original`);
    await mkdir(lock);
    await writeFile(join(lock, "successor"), "preserve");
  });
  expect(await readFile(join(lock, "successor"), "utf8")).toBe("preserve");
});

test("process termination releases the shared-store guard", async () => {
  const root = await directory();
  const child = Bun.spawn([process.execPath, "-e", `
import { locked } from ${JSON.stringify(pathToFileURL(join(import.meta.dir, "../src/common.ts")).href)};
import { writeFile } from "node:fs/promises";
import { join } from "node:path";
const root = ${JSON.stringify(root)};
await locked(root, "interrupted", async () => {
  await writeFile(join(root, "entered"), "");
  await Bun.sleep(60_000);
});
`], { stdout: "ignore", stderr: "pipe" });
  try {
    const deadline = Date.now() + 10_000;
    while (!await exists(join(root, "entered"))) {
      if (Date.now() > deadline || child.exitCode !== null) {
        throw new Error("Lock owner did not enter its callback");
      }
      await Bun.sleep(10);
    }
    child.kill();
    await child.exited;
    expect(await locked(root, "interrupted", async () => 42)).toBe(42);
    expect(await readdir(join(root, "locks"))).toEqual([]);
  } finally {
    if (child.exitCode === null) child.kill();
    await child.exited;
  }
});

test("configuration changes coordinate without provisioning an SDK", async () => {
  const root = await directory();
  const env = { XDG_CONFIG_HOME: join(root, "configuration") };
  const shared = join(root, "shared store");
  expect(await configureRoot(shared, env)).toBe(shared);
  expect((await configuration(env)).root).toBe(shared);
  expect(await exists(shared)).toBe(false);
});
