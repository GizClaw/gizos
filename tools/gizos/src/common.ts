import { access, mkdir, readFile, realpath, rename, rm, stat, writeFile } from "node:fs/promises";
import { constants } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { hostname } from "node:os";
import { randomUUID } from "node:crypto";
import { Database } from "bun:sqlite";

export type Env = Record<string, string | undefined>;
export type InstallationPaths = { sdk?: string; tools?: string; python?: string; path?: string };
export type PathConfiguration = Record<string, Record<string, InstallationPaths>>;
export type Command = (
  args: string[], options?: { cwd?: string; env?: Env; inherit?: boolean },
) => Promise<string>;

export const execute: Command = async (args, options = {}) => {
  const result = Bun.spawn(args, {
    cwd: options.cwd, env: options.env ?? process.env,
    stdin: options.inherit ? "inherit" : "ignore",
    stdout: options.inherit ? "inherit" : "pipe",
    stderr: options.inherit ? "inherit" : "pipe",
  });
  const [stdout, stderr, exitCode] = await Promise.all([
    result.stdout ? new Response(result.stdout).text() : "",
    result.stderr ? new Response(result.stderr).text() : "",
    result.exited,
  ]);
  if (exitCode !== 0) {
    throw new Error(`${basename(args[0])} ${args[1] ?? ""} failed (${exitCode}): ${stderr.trim()}`);
  }
  return stdout.trimEnd();
};

export async function downloadArchive(url: string, path: string, command: Command = execute): Promise<void> {
  await command(["python3", "-c", `
import shutil, ssl, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=60, context=ssl.create_default_context()) as response:
    with open(sys.argv[2], "wb") as output:
        shutil.copyfileobj(response, output)
`, url, path], { inherit: true });
}

export async function exists(path: string): Promise<boolean> {
  try { await access(path); return true; }
  catch (error: any) {
    if (error.code === "ENOENT") return false;
    throw error;
  }
}

export async function installationPath(path: string): Promise<string> {
  try { return await realpath(resolve(path)); }
  catch (error: any) {
    if (error.code !== "ENOENT") throw error;
    return resolve(path);
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
  const guard = join(root, "lockfiles", name);
  await mkdir(dirname(guard), { recursive: true });
  // Keep this inode: unlinking it would let queued callers lock different files.
  const database = new Database(guard, { create: true });
  let held = false;
  try {
    // Waiting in SQLite would block the JS callback that must release the lock.
    database.exec("PRAGMA busy_timeout=0");
    for (;;) {
      try {
        database.exec("BEGIN EXCLUSIVE");
        held = true;
        break;
      } catch (error: any) {
        if (!["SQLITE_BUSY", "SQLITE_LOCKED"].includes(error.code)) throw error;
        await Bun.sleep(100);
      }
    }
    return await directoryLock(root, name, work);
  } finally {
    try { if (held) database.exec("ROLLBACK"); }
    finally { database.close(); }
  }
}

async function directoryLock<T>(root: string, name: string, work: () => Promise<T>): Promise<T> {
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
  const instance = await stat(directory);
  const owner = join(directory, "owner.json");
  const token = randomUUID();
  try {
    await atomicJson(owner, { pid: process.pid, host: hostname(), token });
    return await work();
  } finally {
    const current = await stat(directory).catch((error: any) => {
      if (error.code !== "ENOENT") throw error;
    });
    const currentOwner = await readFile(owner, "utf8").then(JSON.parse).catch((error: any) => {
      if (error.code !== "ENOENT" && !(error instanceof SyntaxError)) throw error;
    });
    if (current?.dev === instance.dev && current?.ino === instance.ino && currentOwner?.token === token) {
      await rm(directory, { recursive: true, force: true });
    }
  }
}

export function quote(value: string): string { return "'" + value.replaceAll("'", "'\\''") + "'"; }
