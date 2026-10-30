import { access, mkdir, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import { constants } from "node:fs";
import { basename, dirname, join } from "node:path";
import { hostname } from "node:os";
import { randomUUID } from "node:crypto";

export type Env = Record<string, string | undefined>;
export type Command = (
  args: string[], options?: { cwd?: string; env?: Env; inherit?: boolean },
) => Promise<string>;

export const execute: Command = async (args, options = {}) => {
  const result = Bun.spawnSync(args, {
    cwd: options.cwd, env: options.env ?? process.env,
    stdin: options.inherit ? "inherit" : "ignore",
    stdout: options.inherit ? "inherit" : "pipe",
    stderr: options.inherit ? "inherit" : "pipe",
  });
  if (result.exitCode !== 0) {
    throw new Error(`${basename(args[0])} ${args[1] ?? ""} failed (${result.exitCode}): ${result.stderr?.toString().trim() ?? ""}`);
  }
  return result.stdout?.toString().trimEnd() ?? "";
};

export async function exists(path: string): Promise<boolean> {
  try { await access(path); return true; }
  catch (error: any) {
    if (error.code === "ENOENT") return false;
    throw error;
  }
}

export async function executable(path: string): Promise<void> {
  try { await access(path, constants.X_OK); }
  catch { throw new Error(`Executable is missing: ${path}`); }
}

export async function atomicJson(path: string, value: unknown): Promise<void> {
  await mkdir(dirname(path), { recursive: true });
  const temporary = `${path}.${randomUUID()}.tmp`;
  try {
    await writeFile(temporary, JSON.stringify(value, null, 2) + "\n", { mode: 0o600 });
    await rename(temporary, path);
  } finally { await rm(temporary, { force: true }); }
}

export async function locked<T>(root: string, name: string, work: () => Promise<T>): Promise<T> {
  const directory = join(root, "locks", name);
  await mkdir(dirname(directory), { recursive: true });
  for (;;) {
    try { await mkdir(directory); break; }
    catch (error: any) {
      if (error.code !== "EEXIST") throw error;
      try {
        const owner = JSON.parse(await readFile(join(directory, "owner.json"), "utf8"));
        if (owner.host === hostname() && Number.isInteger(owner.pid)) {
          try { process.kill(owner.pid, 0); }
          catch (failure: any) {
            if (failure.code === "ESRCH") {
              await rm(directory, { recursive: true, force: true });
              continue;
            }
          }
        }
      } catch (failure: any) {
        if (failure.code !== "ENOENT") throw failure;
        // A process can exit between mkdir and publishing its owner record.
        const age = Date.now() - (await stat(directory).catch(() => ({ mtimeMs: Date.now() }))).mtimeMs;
        if (age > 30_000) {
          await rm(directory, { recursive: true, force: true });
          continue;
        }
      }
      await Bun.sleep(100);
    }
  }
  const owner = join(directory, "owner.json");
  try {
    await atomicJson(owner, { pid: process.pid, host: hostname() });
    return await work();
  } finally { await rm(directory, { recursive: true, force: true }); }
}

export function quote(value: string): string { return "'" + value.replaceAll("'", "'\\''") + "'"; }
