import { AsyncLocalStorage } from "node:async_hooks";
import { lstat, readFile, readdir, rm } from "node:fs/promises";
import { dirname, isAbsolute, join, relative, resolve, sep } from "node:path";
import { hostname } from "node:os";
import { randomUUID } from "node:crypto";
import { atomicJson, execute, exists, installationPath, locked, type Command } from "./common";

const operations = new AsyncLocalStorage<Set<string>>();

export async function withStore<T>(root: string, work: () => Promise<T>): Promise<T> {
  root = resolve(root);
  if (operations.getStore()?.has(root)) return work();
  return locked(root, "maintenance", () => operations.run(new Set([...(operations.getStore() ?? []), root]), work));
}

async function json(path: string): Promise<any | undefined> {
  try { return JSON.parse(await readFile(path, "utf8")); }
  catch (error: any) { if (error.code !== "ENOENT") throw error; }
}

export async function selections(root: string): Promise<Record<string, string>> {
  const value = await json(join(root, "selected.json"));
  if (!value) return {};
  if (value.schema !== 1 || !value.versions || typeof value.versions !== "object" ||
      Array.isArray(value.versions) || Object.values(value.versions).some(version => typeof version !== "string")) {
    throw new Error("Invalid selected environment versions");
  }
  return value.versions;
}

export async function selectVersion(root: string, name: string, version?: string): Promise<void> {
  await withStore(root, async () => {
    const versions = await selections(root);
    if (version) versions[name] = version;
    else delete versions[name];
    await atomicJson(join(root, "selected.json"), { schema: 1, versions });
  });
}

type Artifact = {
  path: string; kind: "sdk" | "tools" | "python" | "download" | "snapshot";
  device: number; inode: number; commit?: string;
};
type Reference = { path: string; contents: boolean };
export type CleanupItem = { path: string; reason: string };
export type CleanupPlan = { remove: CleanupItem[]; preserve: CleanupItem[] };

async function artifacts(root: string): Promise<Artifact[]> {
  const value = await json(join(root, "managed.json"));
  if (!value) return [];
  if (value.schema !== 1 || !Array.isArray(value.paths) ||
      value.paths.some((item: any) => !item || typeof item.path !== "string" || !isAbsolute(item.path) ||
        !["sdk", "tools", "python", "download", "snapshot"].includes(item.kind) ||
        !Number.isSafeInteger(item.device) || !Number.isSafeInteger(item.inode) ||
        item.commit !== undefined && !/^[0-9a-f]{40}$/.test(item.commit))) {
    throw new Error("Invalid managed installation paths");
  }
  return value.paths;
}

export async function own(root: string, path: string, kind: Artifact["kind"], commit?: string): Promise<void> {
  await withStore(root, async () => {
    path = await installationPath(path);
    const info = await lstat(path);
    const paths = (await artifacts(root)).filter(item => item.path !== path);
    paths.push({ path, kind, device: info.dev, inode: info.ino, ...(commit ? { commit } : {}) });
    await atomicJson(join(root, "managed.json"), { schema: 1, paths });
  });
}

export async function records(root: string, folder: "environments" | "resources"): Promise<{ path: string; value: any }[]> {
  const output: { path: string; value: any }[] = [];
  const base = join(root, folder);
  const names = await readdir(base, { withFileTypes: true }).catch((error: any) => {
    if (error.code !== "ENOENT") throw error;
    return [];
  });
  for (const name of names) {
    if (!name.isDirectory()) continue;
    for (const file of await readdir(join(base, name.name), { withFileTypes: true })) {
      if (!file.isFile() || !file.name.endsWith(".json")) continue;
      const path = join(base, name.name, file.name);
      const value = await json(path);
      const paths = folder === "environments" ? [value?.sdk, value?.tools] : [value?.root];
      if (value?.schema !== 1 || typeof value.version !== "string" ||
          paths.some(item => typeof item !== "string" || !isAbsolute(item))) {
        throw new Error(`Invalid installation record: ${path}`);
      }
      output.push({ path, value });
    }
  }
  return output;
}

function within(path: string, parent: string): boolean {
  const suffix = relative(parent, path);
  return suffix === "" || !suffix.startsWith(`..${sep}`) && suffix !== ".." && !isAbsolute(suffix);
}

async function references(root: string, excluding: string[]): Promise<Reference[]> {
  const result: Reference[] = [];
  const add = async (path: unknown, contents = true) => {
    if (typeof path !== "string" || !isAbsolute(path)) throw new Error("Invalid installation reference");
    result.push({ path: await installationPath(path), contents });
  };
  for (const { path, value } of await records(root, "environments")) {
    if (excluding.includes(path)) continue;
    await add(value.sdk);
    await add(value.tools, false);
    if (value.python) await add(dirname(dirname(value.python)));
    for (const path of value.dependencies ?? []) await add(path);
  }
  for (const { path, value } of await records(root, "resources")) {
    if (!excluding.includes(path)) await add(value.root);
  }
  const leases = join(root, "leases");
  for (const name of await readdir(leases).catch((error: any) => {
    if (error.code !== "ENOENT") throw error;
    return [];
  })) {
    if (!name.endsWith(".json")) continue;
    const lease = await json(join(leases, name));
    if (!lease || !Number.isSafeInteger(lease.pid) || !Array.isArray(lease.paths)) throw new Error("Invalid active environment lease");
    if (lease.host === hostname()) {
      try { process.kill(lease.pid, 0); }
      catch (error: any) { if (error.code === "ESRCH") continue; throw error; }
    }
    for (const path of lease.paths) await add(path);
  }
  return result;
}

export async function cleanupPlan(
  root: string, excluding: string[] = [], candidates?: string[], command: Command = execute,
): Promise<CleanupPlan> {
  const refs = await references(root, excluding);
  const plan: CleanupPlan = { remove: [], preserve: [] };
  for (const item of await artifacts(root)) {
    if (candidates && !candidates.includes(item.path)) continue;
    const preserve = (reason: string) => plan.preserve.push({ path: item.path, reason });
    if (within(root, item.path) || item.path === dirname(item.path)) { preserve("contains the shared store"); continue; }
    if (refs.some(ref => within(ref.path, item.path) || ref.contents && within(item.path, ref.path))) {
      preserve("referenced by an installed environment or running command"); continue;
    }
    let info;
    try { info = await lstat(item.path); }
    catch (error: any) { if (error.code !== "ENOENT") throw error; }
    if (info && (info.isSymbolicLink() || info.dev !== item.device || info.ino !== item.inode ||
        await installationPath(item.path) !== item.path)) { preserve("path was replaced"); continue; }
    if (info && (item.kind === "sdk" || item.kind === "snapshot")) {
      try {
        const git = (...args: string[]) => command(["git", "-c", "core.hooksPath=/dev/null", "-C", item.path, ...args]);
        if ((await git("rev-parse", "HEAD")).trim() !== item.commit ||
            (await git("status", "--porcelain=v1", "--untracked-files=normal", "--ignore-submodules=none")).trim() ||
            (await git("submodule", "status", "--recursive")).split("\n").some(line => /^[+U]/.test(line))) {
          preserve("source has local changes"); continue;
        }
      } catch { preserve("source cannot be verified"); continue; }
    }
    plan.remove.push({ path: item.path, reason: info ? `unused managed ${item.kind}` : "already absent" });
  }
  if (candidates) {
    const known = new Set([...plan.remove, ...plan.preserve].map(item => item.path));
    for (const path of candidates) if (!known.has(path)) plan.preserve.push({ path, reason: "adopted or ownership is unknown" });
  }
  // A retained child also protects an otherwise unreferenced parent directory.
  for (const item of [...plan.remove]) {
    if (plan.preserve.some(child => within(child.path, item.path))) {
      plan.remove.splice(plan.remove.indexOf(item), 1);
      plan.preserve.push({ path: item.path, reason: "contains a retained path" });
    }
  }
  return plan;
}

export async function applyCleanup(root: string, plan: CleanupPlan): Promise<void> {
  const owned = await artifacts(root);
  for (const item of plan.remove) {
    const expected = owned.find(entry => entry.path === item.path);
    const info = await lstat(item.path).catch((error: any) => {
      if (error.code !== "ENOENT") throw error;
      return undefined;
    });
    if (!expected || info && (info.isSymbolicLink() || info.dev !== expected.device || info.ino !== expected.inode ||
        await installationPath(item.path) !== item.path)) throw new Error(`Path changed during cleanup; preserved: ${item.path}`);
  }
  for (const item of plan.remove) await rm(item.path, { recursive: true, force: true });
  const deleted = new Set(plan.remove.map(item => item.path));
  await atomicJson(join(root, "managed.json"), { schema: 1, paths: owned.filter(item => !deleted.has(item.path)) });
}

export async function withLease<T>(root: string, paths: string[] | (() => Promise<string[]>), work: () => Promise<T>): Promise<T> {
  const path = join(root, "leases", `${process.pid}-${randomUUID()}.json`);
  await withStore(root, async () => atomicJson(path, {
    host: hostname(), pid: process.pid, paths: typeof paths === "function" ? await paths() : paths,
  }));
  try { return await work(); }
  finally { await withStore(root, () => rm(path, { force: true })); }
}
