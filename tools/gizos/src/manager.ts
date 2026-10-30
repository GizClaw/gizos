import {
  access, mkdir, readFile, readdir, realpath, rename, rm, stat, writeFile,
} from "node:fs/promises";
import { constants, readFileSync } from "node:fs";
import { homedir, hostname } from "node:os";
import { basename, dirname, isAbsolute, join, resolve, sep } from "node:path";
import { createHash, randomUUID } from "node:crypto";
import bundledCatalog from "./catalog.json";

export type Kind = "idf" | "bk";
export type Sdk = {
  kind: Kind;
  version: string;
  repository: string;
  commit: string;
  requiredFile: string;
  pythonSeries?: string;
  targets?: string[];
  default?: boolean;
};
export type Installation = {
  schema: 1;
  kind: Kind;
  version: string;
  commit: string;
  sdk: string;
  tools: string;
  python?: string;
  ready: boolean;
  targets: string[];
};
export type Options = {
  sourcesOnly?: boolean;
  targets?: string[];
  sdk?: string;
  tools?: string;
  python?: string;
};
type Env = Record<string, string | undefined>;
export type Command = (
  args: string[], options?: { cwd?: string; env?: Env; inherit?: boolean },
) => Promise<string>;
type Catalog = typeof bundledCatalog & { sdks: Sdk[] };
export const catalog = bundledCatalog as Catalog;
export const PACKAGE = "@gizclaw/gizos";
export const REGISTRY = "https://static-volc.gizclaw.com/npm/";
const controlled = [
  "IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH", "IDF_DEACTIVATE_FILE_PATH", "H2LOADER_IDF_PATH",
  "ESP_IDF_VERSION", "ESP_ROM_ELF_DIR", "OPENOCD_SCRIPTS",
  "BK7258_PATH", "BK_SDK_DIR", "COMPILER_TOOLCHAIN_PATH", "GIZOS_BK_TOOLCHAIN_ROOT",
];

export function kind(value: string): Kind {
  if (value === "esp" || value === "idf") return "idf";
  if (value === "bk" || value === "bk7258") return "bk";
  throw new Error(`Unknown SDK ${value}; use idf or bk`);
}

export function selectSdk(name: string, version: string, entries: Sdk[] = catalog.sdks): Sdk {
  const family = kind(name);
  const sdk = entries.find(item => item.kind === family && item.version === version);
  if (!sdk) throw new Error(`Unsupported ${family} version ${version}; run gizos list ${family}`);
  if (!/^[0-9a-f]{40}$/.test(sdk.commit)) throw new Error("Invalid SDK catalog commit");
  return sdk;
}

export function configFile(env: Env = process.env): string {
  const base = env.XDG_CONFIG_HOME || (
    process.platform === "darwin"
      ? join(homedir(), "Library", "Application Support")
      : join(homedir(), ".config")
  );
  return join(base, "gizos", "devenv.json");
}

export function defaultRoot(env: Env = process.env): string {
  const base = env.XDG_DATA_HOME || (
    process.platform === "darwin"
      ? join(homedir(), "Library", "Application Support")
      : join(homedir(), ".local", "share")
  );
  return join(base, "gizos", "devenv");
}

export async function dataRoot(explicit?: string, env: Env = process.env): Promise<string> {
  if (explicit || env.GIZOS_DEVENV_HOME) return resolve(explicit || env.GIZOS_DEVENV_HOME!);
  try {
    const config = JSON.parse(await readFile(configFile(env), "utf8"));
    if (typeof config.root !== "string" || !isAbsolute(config.root)) {
      throw new Error("Invalid gizos root configuration");
    }
    return resolve(config.root);
  } catch (error: any) {
    if (error.code !== "ENOENT") throw error;
  }
  return defaultRoot(env);
}

export async function configureRoot(value: string, env: Env = process.env): Promise<string> {
  const root = resolve(value);
  const file = configFile(env);
  await mkdir(dirname(file), { recursive: true });
  await atomicJson(file, { schema: 1, root });
  return root;
}

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

async function executable(path: string): Promise<void> {
  try { await access(path, constants.X_OK); }
  catch { throw new Error(`Executable is missing: ${path}`); }
}

async function atomicJson(path: string, value: unknown): Promise<void> {
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

export function cleanEnvironment(source: Env = process.env): Env {
  const environment = { ...source };
  const roots = [
    source.IDF_PATH, source.IDF_TOOLS_PATH, source.IDF_PYTHON_ENV_PATH,
    source.COMPILER_TOOLCHAIN_PATH, source.VIRTUAL_ENV,
  ].filter((value): value is string => Boolean(value));
  environment.PATH = (source.PATH ?? "").split(process.platform === "win32" ? ";" : ":")
    .filter(entry => entry && !roots.some(root => {
      const parent = resolve(root);
      const candidate = resolve(entry);
      return candidate === parent || candidate.startsWith(parent + sep);
    })).join(process.platform === "win32" ? ";" : ":");
  for (const key of Object.keys(environment)) {
    if (key.startsWith("IDF_") || controlled.includes(key) ||
        ["VIRTUAL_ENV", "PYTHONHOME", "PYTHONPATH"].includes(key)) delete environment[key];
  }
  return environment;
}

export class Manager {
  constructor(
    public root: string,
    public command: Command = execute,
    public entries: Sdk[] = catalog.sdks,
  ) {}

  sdk(name: string, version: string): Sdk { return selectSdk(name, version, this.entries); }
  sourcePath(sdk: Sdk): string {
    return join(this.root, "sdks", sdk.kind, `${sdk.version}-${sdk.commit}`);
  }
  poolFile(sdk: Sdk): string {
    const version = sdk.kind === "bk" ? `${catalog.arm.version}-` : "";
    return join(this.root, "pools", `${sdk.kind}-${version}${process.platform}-${process.arch}.json`);
  }
  toolsPath(sdk: Sdk): string {
    try {
      const pool = JSON.parse(readFileSync(this.poolFile(sdk), "utf8"));
      if (pool.schema !== 1 || typeof pool.tools !== "string" || !isAbsolute(pool.tools)) {
        throw new Error(`Invalid shared tool pool: ${this.poolFile(sdk)}`);
      }
      return pool.tools;
    } catch (error: any) {
      if (error.code !== "ENOENT") throw error;
    }
    return sdk.kind === "idf"
      ? join(this.root, "tools", "idf", `${process.platform}-${process.arch}`)
      : join(this.root, "tools", "arm", `${catalog.arm.version}-${process.platform}-${process.arch}`);
  }
  statePath(sdk: Sdk): string {
    return join(this.root, "environments", sdk.kind, `${sdk.version}.json`);
  }
  async record(sdk: Sdk): Promise<Installation | undefined> {
    try {
      const item = JSON.parse(await readFile(this.statePath(sdk), "utf8")) as Installation;
      if (item.schema !== 1 || item.kind !== sdk.kind || item.version !== sdk.version ||
          item.commit !== sdk.commit || !isAbsolute(item.sdk) || !isAbsolute(item.tools) ||
          (item.python !== undefined && !isAbsolute(item.python))) {
        throw new Error(`Invalid installation record: ${this.statePath(sdk)}`);
      }
      return item;
    } catch (error: any) {
      if (error.code === "ENOENT") return undefined;
      throw error;
    }
  }
  async git(path: string, ...args: string[]): Promise<string> {
    return this.command(["git", "-c", "core.hooksPath=/dev/null", "-C", path, ...args], {
      env: { ...process.env, GIT_TERMINAL_PROMPT: "0" },
    });
  }
  async verifySource(sdk: Sdk, path: string): Promise<void> {
    if (!await exists(join(path, sdk.requiredFile))) throw new Error(`SDK input is missing: ${path}`);
    if ((await this.git(path, "rev-parse", "HEAD")).trim() !== sdk.commit) {
      throw new Error(`SDK commit mismatch: ${path}; existing files were preserved`);
    }
    if ((await this.git(path, "status", "--porcelain=v1", "--untracked-files=no")).trim()) {
      throw new Error(`SDK has tracked changes: ${path}; existing files were preserved`);
    }
    const submodules = await this.git(path, "submodule", "status", "--recursive");
    if (submodules.split("\n").some(line => /^[-+U]/.test(line))) {
      throw new Error(`SDK submodules are incomplete or changed: ${path}`);
    }
  }
  async fetchSource(sdk: Sdk, path: string): Promise<void> {
    if (await exists(path)) return this.verifySource(sdk, path);
    await mkdir(dirname(path), { recursive: true });
    const temporary = `${path}.${randomUUID()}.tmp`;
    await mkdir(temporary);
    try {
      await this.git(temporary, "init", "--quiet");
      await this.git(temporary, "remote", "add", "origin", sdk.repository);
      await this.git(temporary, "fetch", "--depth=1", "origin", sdk.commit);
      await this.git(temporary, "checkout", "--quiet", "--detach", "FETCH_HEAD");
      await this.git(temporary, "submodule", "update", "--init", "--recursive", "--depth=1");
      await this.verifySource(sdk, temporary);
      await rename(temporary, path);
    } finally { await rm(temporary, { recursive: true, force: true }); }
  }
  async findPython(sdk: Sdk, tools: string): Promise<string> {
    const base = join(tools, "python_env");
    const names = await readdir(base).catch((error: any) => {
      if (error.code === "ENOENT") return [];
      throw error;
    });
    const matches: string[] = [];
    for (const name of names) {
      const python = join(base, name, "bin", "python");
      if (name.startsWith(`idf${sdk.pythonSeries}_py`) && name.endsWith("_env") && await exists(python)) {
        matches.push(python);
      }
    }
    if (matches.length !== 1) {
      throw new Error(`Expected one IDF ${sdk.version} Python environment in ${base}; use --python to select one`);
    }
    return matches[0];
  }
  async verifyArm(tools: string): Promise<void> {
    const compiler = join(tools, "bin", "arm-none-eabi-gcc");
    await executable(compiler);
    const version = await this.command([compiler, "-dumpfullversion", "-dumpversion"]);
    if (version.trim() !== catalog.arm.compilerVersion) {
      throw new Error(`GNU Arm compiler mismatch: ${compiler}`);
    }
    for (const tool of ["arm-none-eabi-gcc-ar", "arm-none-eabi-objcopy"]) {
      await executable(join(tools, "bin", tool));
    }
  }
  async installArm(tools: string): Promise<void> {
    if (await exists(tools)) return this.verifyArm(tools);
    if (process.platform !== "darwin" && !(process.platform === "linux" && process.arch === "x64")) {
      throw new Error("GNU Arm 10.3 supports macOS and Linux x86_64; Apple Silicon requires Rosetta 2");
    }
    const distribution = catalog.arm.hosts[process.platform as "darwin" | "linux"];
    const cache = join(this.root, "downloads");
    await mkdir(cache, { recursive: true });
    const archive = join(cache, `${distribution.sha256}-${basename(distribution.url)}`);
    const digest = async (path: string) => {
      const hash = createHash("sha256");
      for await (const chunk of Bun.file(path).stream()) hash.update(chunk);
      return hash.digest("hex");
    };
    if (!await exists(archive)) {
      const response = await fetch(distribution.url);
      if (!response.ok) throw new Error(`GNU Arm download failed: HTTP ${response.status}`);
      const partial = `${archive}.${randomUUID()}.tmp`;
      try {
        await Bun.write(partial, response);
        if (await digest(partial) !== distribution.sha256) throw new Error("GNU Arm archive checksum mismatch");
        await rename(partial, archive);
      } finally { await rm(partial, { force: true }); }
    }
    if (await digest(archive) !== distribution.sha256) throw new Error(`Corrupt cached archive: ${archive}`);
    const temporary = `${tools}.${randomUUID()}.tmp`;
    await mkdir(temporary, { recursive: true });
    try {
      await this.command(["tar", "-xjf", archive, "--strip-components=1", "-C", temporary]);
      await this.verifyArm(temporary);
      await rename(temporary, tools);
    } finally { await rm(temporary, { recursive: true, force: true }); }
  }
  async setup(sdk: Sdk, options: Options = {}, adopt = false): Promise<Installation> {
    if (!["darwin", "linux"].includes(process.platform)) throw new Error("SDK setup currently supports macOS and Linux");
    return locked(this.root, `${sdk.kind}-${sdk.version}`, async () => {
      const previous = await this.record(sdk);
      const item: Installation = {
        schema: 1, kind: sdk.kind, version: sdk.version, commit: sdk.commit,
        sdk: options.sdk ? await realpath(resolve(options.sdk)) : previous?.sdk ?? this.sourcePath(sdk),
        tools: options.tools ? await realpath(resolve(options.tools)) : previous?.tools ?? this.toolsPath(sdk),
        // A virtualenv's Python can be a symlink to the host interpreter.
        python: options.python ? resolve(options.python) : previous?.python,
        ready: previous?.ready ?? false,
        targets: options.targets ?? previous?.targets ?? sdk.targets ?? [],
      };
      if (adopt) await this.verifySource(sdk, item.sdk);
      else await this.fetchSource(sdk, item.sdk);
      if (!options.sourcesOnly) {
        // IDF owns the family/version layout. All SDK versions use the same
        // tools root, so identical compiler archives and installations are shared.
        await locked(this.root, `${sdk.kind}-tools`, async () => {
          if (!options.tools && !previous?.ready) item.tools = this.toolsPath(sdk);
          if (sdk.kind === "idf") {
            if (!adopt) {
              const environment = cleanEnvironment();
              environment.IDF_TOOLS_PATH = item.tools;
              if (item.python) environment.IDF_PYTHON_ENV_PATH = dirname(dirname(item.python));
              await this.command(["bash", join(item.sdk, "install.sh"), item.targets.join(",")], {
                cwd: item.sdk, env: environment, inherit: true,
              });
            }
            item.python = item.python ?? await this.findPython(sdk, item.tools);
            await executable(item.python);
          } else if (adopt) await this.verifyArm(item.tools);
          else await this.installArm(item.tools);
          item.ready = true;
          await this.environment(sdk, item);
          if (!await exists(this.poolFile(sdk))) {
            await atomicJson(this.poolFile(sdk), { schema: 1, tools: item.tools });
          }
        });
      } else if (item.ready) await this.environment(sdk, item);
      await atomicJson(this.statePath(sdk), item);
      return item;
    });
  }
  async environment(sdk: Sdk, selected?: Installation, inherited: Env = process.env): Promise<Env> {
    const item = selected ?? await this.record(sdk);
    if (!item?.ready) throw new Error(`${sdk.kind} ${sdk.version} is not installed; run gizos setup ${sdk.kind} ${sdk.version}`);
    await this.verifySource(sdk, item.sdk);
    const environment = cleanEnvironment(inherited);
    environment.GIZOS_DEVENV_HOME = this.root;
    if (sdk.kind === "bk") {
      await this.verifyArm(item.tools);
      Object.assign(environment, {
        BK7258_PATH: item.sdk, BK_SDK_DIR: item.sdk,
        COMPILER_TOOLCHAIN_PATH: join(item.tools, "bin"),
        GIZOS_BK_TOOLCHAIN_ROOT: item.tools,
        PATH: `${join(item.tools, "bin")}:${environment.PATH ?? ""}`,
      });
    } else {
      await executable(item.python!);
      Object.assign(environment, {
        IDF_PATH: item.sdk, IDF_TOOLS_PATH: item.tools,
        IDF_PYTHON_ENV_PATH: dirname(dirname(item.python!)),
      });
      const output = await this.command(
        [item.python!, join(item.sdk, "tools/idf_tools.py"), "export", "--format", "key-value"],
        { cwd: item.sdk, env: environment },
      );
      for (const line of output.split("\n")) {
        const match = /^([A-Z][A-Z0-9_]*)=(.*)$/.exec(line);
        if (!match) continue;
        if (match[1] === "PATH") {
          environment.PATH = match[2].replace(/\$\{PATH\}|\$PATH/g, environment.PATH ?? "");
        } else environment[match[1]] = match[2];
      }
      environment.H2LOADER_IDF_PATH = item.sdk;
    }
    const bk = this.entries.find(entry => entry.kind === "bk" && entry.default);
    if (!environment.GIZOS_BK_TOOLCHAIN_ROOT && bk) {
      const arm = this.toolsPath(bk);
      if (await exists(join(arm, "bin", "arm-none-eabi-gcc"))) {
        environment.GIZOS_BK_TOOLCHAIN_ROOT = arm;
      }
    }
    return environment;
  }
  async describe(sdk: Sdk) {
    const item = await this.record(sdk);
    const sourceExists = item && await exists(join(item.sdk, sdk.requiredFile));
    const toolsExist = item && await exists(item.tools) &&
      (sdk.kind !== "idf" || item.python && await exists(item.python));
    return {
      kind: sdk.kind, version: sdk.version, commit: sdk.commit,
      status: !item ? "not installed" : !sourceExists ? "SDK missing"
        : !item.ready ? "sources only" : !toolsExist ? "tools missing" : "installed",
      sdk: item?.sdk ?? this.sourcePath(sdk),
      tools: item?.tools ?? this.toolsPath(sdk),
      python: item?.python ?? null,
      loader: sdk.kind === "bk" ? process.env.BK_LOADER_BIN ?? null : undefined,
    };
  }
  async defaultEnvironment(inherited: Env = process.env): Promise<Env> {
    const installed: Sdk[] = [];
    for (const sdk of this.entries.filter(item => item.default)) {
      if ((await this.record(sdk))?.ready) installed.push(sdk);
    }
    if (!installed.length) return { ...inherited, GIZOS_DEVENV_HOME: this.root };
    const environment = cleanEnvironment(inherited);
    environment.GIZOS_DEVENV_HOME = this.root;
    const hostPaths = (environment.PATH ?? "").split(":");
    const paths: string[] = [];
    for (const sdk of installed) {
      const selected = await this.environment(sdk, undefined, inherited);
      if (selected.GIZOS_BK_TOOLCHAIN_ROOT) {
        environment.GIZOS_BK_TOOLCHAIN_ROOT = selected.GIZOS_BK_TOOLCHAIN_ROOT;
      }
      for (const key of controlled) {
        const belongs = sdk.kind === "bk"
          ? ["BK7258_PATH", "BK_SDK_DIR", "COMPILER_TOOLCHAIN_PATH", "GIZOS_BK_TOOLCHAIN_ROOT"].includes(key)
          : !["BK7258_PATH", "BK_SDK_DIR", "COMPILER_TOOLCHAIN_PATH", "GIZOS_BK_TOOLCHAIN_ROOT"].includes(key);
        if (belongs && selected[key] !== undefined) environment[key] = selected[key];
      }
      paths.push(...(selected.PATH ?? "").split(":").filter(path => !hostPaths.includes(path)));
    }
    environment.PATH = [...new Set([...paths, ...hostPaths].filter(Boolean))].join(":");
    return environment;
  }
}

export function quote(value: string): string { return "'" + value.replaceAll("'", "'\\''") + "'"; }

export function shellEnvironment(environment: Env, inherited: Env = process.env): string {
  const keys = new Set([...controlled, "H2LOADER_IDF_PATH", "PATH"]);
  for (const key of Object.keys(environment)) {
    if (environment[key] !== inherited[key] && /^[A-Z][A-Z0-9_]*$/.test(key)) keys.add(key);
  }
  return [...keys].sort().map(key => environment[key] === undefined
    ? `unset ${key}` : `export ${key}=${quote(environment[key]!)}`).join("\n");
}

export async function upgrade(version = "latest", command: Command = execute): Promise<void> {
  if (version !== "latest" && !/^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$/.test(version)) {
    throw new Error("Upgrade version must be latest or an exact package version");
  }
  await command([
    process.execPath, "add", "--global", `${PACKAGE}@${version}`, "--registry", REGISTRY,
  ], { inherit: true });
}
