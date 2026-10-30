import {
  mkdir, readFile, readdir, realpath, rename, rm,
} from "node:fs/promises";
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { basename, dirname, isAbsolute, join, resolve, sep } from "node:path";
import { createHash, randomUUID } from "node:crypto";
import bundledCatalog from "./catalog.json";
import { Resources, resourceVariables, type Tool } from "./resources";
import { atomicJson, execute, executable, exists, locked, quote, type Command, type Env } from "./common";
export { execute, exists, locked, type Command } from "./common";

export type Kind = "idf" | "bk" | "ac695n" | "ac707n" | "ac791n";
export type Sdk = {
  kind: Kind;
  version: string;
  repository: string;
  commit: string;
  requiredFile: string;
  pythonSeries?: string;
  targets?: string[];
  default?: boolean;
  requiredFiles?: string[];
  sdkSubdirectory?: string;
  lfs?: boolean;
};
export type Installation = {
  schema: 1;
  name?: string;
  repository?: string;
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
  postbuild?: string;
};
type Catalog = typeof bundledCatalog & { sdks: Sdk[] };
export const catalog = bundledCatalog as Catalog;
export const PACKAGE = "@gizclaw/gizos";
export const REGISTRY = "https://static-volc.gizclaw.com/npm/";
const controlled = [
  "IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH", "IDF_DEACTIVATE_FILE_PATH", "H2LOADER_IDF_PATH",
  "ESP_IDF_VERSION", "ESP_ROM_ELF_DIR", "OPENOCD_SCRIPTS",
  "BK7258_PATH", "BK_SDK_DIR", "COMPILER_TOOLCHAIN_PATH", "GIZOS_BK_TOOLCHAIN_ROOT",
  ...resourceVariables, "VIRTUAL_ENV", "GIZOS_ENV_PATHS",
  "H2LOADER_PYTHON", "H2LOADER_BOOTSTRAP_PYTHON", "BK3633_SDK_PATH", "BK3633_BINCONVERTER_BIN",
];
const sdkVariables: Record<string, string[]> = {
  idf: ["IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH", "H2LOADER_IDF_PATH", "ESP_IDF_VERSION", "ESP_ROM_ELF_DIR", "OPENOCD_SCRIPTS", "VIRTUAL_ENV"],
  bk: ["BK7258_PATH", "BK_SDK_DIR", "COMPILER_TOOLCHAIN_PATH", "GIZOS_BK_TOOLCHAIN_ROOT"],
  ac695n: ["JIELI_AC695N_SDK_PATH"],
  ac707n: ["JIELI_AC707N_SDK_PATH"],
  ac791n: ["JIELI_AC791N_SDK_PATH"],
};

function usesArm(sdk: Sdk): boolean { return sdk.kind === "bk"; }

function markPaths(environment: Env, baseline: Env): Env {
  const original = new Set((baseline.PATH ?? "").split(":"));
  const paths = [...new Set((environment.PATH ?? "").split(":").filter(Boolean))];
  environment.PATH = paths.join(":");
  environment.GIZOS_ENV_PATHS = paths.filter(path => !original.has(path)).join(":");
  return environment;
}

export function kind(value: string): Kind {
  value = value.toLowerCase();
  if (value === "esp" || value === "idf") return "idf";
  if (value === "bk" || value === "bk7258") return "bk";
  if (["ac695n", "ac707n", "ac791n"].includes(value)) return value as Kind;
  throw new Error(`Unknown SDK ${value}; run gizos list`);
}

export function sdkName(sdk: Sdk): string {
  const name = basename(sdk.repository.replace(/\/$/, "").replace(/\.git$/, ""));
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name)) throw new Error("Invalid SDK repository name");
  return name;
}

export function sdkChoices(name?: string, entries: Sdk[] = catalog.sdks): Sdk[] {
  if (!name) return entries;
  const named = entries.filter(sdk => sdkName(sdk).toLowerCase() === name.toLowerCase());
  return named.length ? named : entries.filter(sdk => sdk.kind === kind(name));
}

export function selectSdk(name: string, version: string, entries: Sdk[] = catalog.sdks): Sdk {
  const matches = sdkChoices(name, entries).filter(item => item.version === version);
  if (matches.length > 1) {
    throw new Error(`Ambiguous SDK ${name} ${version}; use a repository name: ${matches.map(sdkName).join(", ")}`);
  }
  const sdk = matches[0];
  if (!sdk) throw new Error(`Unsupported ${name} version ${version}; run gizos list ${name}`);
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

export function cleanEnvironment(source: Env = process.env): Env {
  const environment = { ...source };
  const roots = [
    source.IDF_PATH, source.IDF_TOOLS_PATH, source.IDF_PYTHON_ENV_PATH,
    source.COMPILER_TOOLCHAIN_PATH, source.VIRTUAL_ENV,
    source.ADB && basename(dirname(source.ADB)) === "platform-tools" ? dirname(source.ADB) : undefined,
  ].filter((value): value is string => Boolean(value));
  const managedPaths = new Set((source.GIZOS_ENV_PATHS ?? "").split(":").filter(Boolean));
  environment.PATH = (source.PATH ?? "").split(process.platform === "win32" ? ";" : ":")
    .filter(entry => entry && !managedPaths.has(entry) && !roots.some(root => {
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

  get resources(): Resources { return new Resources(this.root, this.command); }
  sdk(name: string, version: string): Sdk { return selectSdk(name, version, this.entries); }
  sdks(name?: string): Sdk[] { return sdkChoices(name, this.entries); }
  sourcePath(sdk: Sdk): string {
    return join(this.root, "sdks", sdkName(sdk), `${sdk.version}-${sdk.commit}`);
  }
  poolFile(sdk: Sdk): string {
    const version = usesArm(sdk) ? `${catalog.arm.version}-` : "";
    const family = usesArm(sdk) ? "bk" : sdk.kind;
    return join(this.root, "pools", `${family}-${version}${process.platform}-${process.arch}.json`);
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
    if (sdk.kind.startsWith("ac")) {
      return join(this.root, "tools", "jieli-toolchain", `20250805.1-${process.platform}-${process.arch}`);
    }
    return sdk.kind === "idf"
      ? join(this.root, "tools", "idf", `${process.platform}-${process.arch}`)
      : join(this.root, "tools", "arm", `${catalog.arm.version}-${process.platform}-${process.arch}`);
  }
  statePath(sdk: Sdk): string {
    return join(this.root, "environments", sdkName(sdk), `${sdk.version}.json`);
  }
  async record(sdk: Sdk): Promise<Installation | undefined> {
    const paths = [...new Set([
      this.statePath(sdk), join(this.root, "environments", sdk.kind, `${sdk.version}.json`),
    ])];
    for (const path of paths) {
      try {
        const item = JSON.parse(await readFile(path, "utf8")) as Installation;
        // A family record can belong to another explicitly catalogued repository.
        if (path !== this.statePath(sdk) && item.schema === 1 && item.kind === sdk.kind &&
            item.version === sdk.version && item.commit !== sdk.commit &&
            this.entries.some(other => other.kind === sdk.kind && other.version === sdk.version &&
              other.commit === item.commit && sdkName(other) !== sdkName(sdk))) continue;
        if (item.schema !== 1 || item.kind !== sdk.kind || item.version !== sdk.version ||
            item.commit !== sdk.commit || !isAbsolute(item.sdk) || !isAbsolute(item.tools) ||
            (item.name !== undefined && item.name !== sdkName(sdk)) ||
            (item.repository !== undefined && typeof item.repository !== "string") ||
            (item.python !== undefined && !isAbsolute(item.python))) {
          throw new Error(`Invalid installation record: ${path}`);
        }
        return item;
      } catch (error: any) {
        if (error.code !== "ENOENT") throw error;
      }
    }
  }
  async git(path: string, ...args: string[]): Promise<string> {
    return this.command(["git", "-c", "core.hooksPath=/dev/null", "-C", path, ...args], {
      env: { ...process.env, GIT_TERMINAL_PROMPT: "0" },
    });
  }
  async verifySource(sdk: Sdk, path: string): Promise<void> {
    if (!await exists(join(path, sdk.requiredFile))) throw new Error(`SDK input is missing: ${path}`);
    for (const file of sdk.requiredFiles ?? []) {
      if (!await exists(join(path, file))) throw new Error(`SDK input is missing: ${join(path, file)}`);
    }
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
    if (sdk.lfs && (await this.git(path, "lfs", "ls-files")).split("\n").some(line => /^[a-f0-9]+ - /.test(line))) {
      throw new Error(`SDK has unresolved Git LFS objects: ${path}`);
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
      if (sdk.lfs) await this.git(temporary, "lfs", "pull");
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
    return locked(this.root, `${sdkName(sdk)}-${sdk.version}`, async () => {
      const previous = await this.record(sdk);
      const item: Installation = {
        schema: 1, name: sdkName(sdk), repository: sdk.repository,
        kind: sdk.kind, version: sdk.version, commit: sdk.commit,
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
        await locked(this.root, `${usesArm(sdk) ? "bk" : sdk.kind}-tools`, async () => {
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
          } else if (usesArm(sdk)) {
            if (adopt) await this.verifyArm(item.tools);
            else await this.installArm(item.tools);
            if (!adopt) await this.resources.setup(this.resources.select("bk-loader"));
          } else {
            const toolchain = this.resources.select("jieli-toolchain");
            const postbuild = this.resources.select("jieli-postbuild");
            const existing = await this.resources.record(toolchain);
            const existingPostbuild = await this.resources.record(postbuild);
            if (adopt && (!options.tools && !existing || !options.postbuild && !existingPostbuild)) {
              throw new Error("adopt requires registered JieLi tools or --tools and --postbuild paths");
            }
            item.tools = (await this.resources.setup(toolchain, options.tools)).root;
            await this.resources.setup(postbuild, options.postbuild);
          }
          item.ready = true;
          await this.environment(sdk, item);
          if (!sdk.kind.startsWith("ac") && !await exists(this.poolFile(sdk))) {
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
    if (!item?.ready) throw new Error(`${sdkName(sdk)} ${sdk.version} is not installed; run gizos setup ${sdkName(sdk)} ${sdk.version}`);
    await this.verifySource(sdk, item.sdk);
    const baseline = cleanEnvironment(inherited);
    const environment = await this.resources.environment(baseline);
    environment.GIZOS_DEVENV_HOME = this.root;
    if (usesArm(sdk)) {
      await this.verifyArm(item.tools);
      Object.assign(environment, {
        COMPILER_TOOLCHAIN_PATH: join(item.tools, "bin"),
        GIZOS_BK_TOOLCHAIN_ROOT: item.tools,
        PATH: `${join(item.tools, "bin")}:${environment.PATH ?? ""}`,
      });
      Object.assign(environment, { BK7258_PATH: item.sdk, BK_SDK_DIR: item.sdk });
    } else if (sdk.kind === "idf") {
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
      environment.PATH = `${dirname(item.python!)}:${join(item.sdk, "tools")}:${environment.PATH ?? ""}`;
      environment.VIRTUAL_ENV = dirname(dirname(item.python!));
    } else {
      environment[sdkVariables[sdk.kind][0]] = resolve(item.sdk, sdk.sdkSubdirectory ?? ".");
      environment.JIELI_TOOLCHAIN_ROOT = item.tools;
      if (!environment.JIELI_POSTBUILD_ROOT) throw new Error("JieLi post-build tools are not installed");
    }
    const bk = this.entries.find(entry => entry.kind === "bk" && entry.default);
    if (!environment.GIZOS_BK_TOOLCHAIN_ROOT && bk) {
      const arm = this.toolsPath(bk);
      if (await exists(join(arm, "bin", "arm-none-eabi-gcc"))) {
        environment.GIZOS_BK_TOOLCHAIN_ROOT = arm;
      }
    }
    // An explicitly supplied loader remains usable before it is adopted.
    if (!environment.BK_LOADER_BIN && inherited.BK_LOADER_BIN) {
      await executable(inherited.BK_LOADER_BIN);
      environment.BK_LOADER_BIN = inherited.BK_LOADER_BIN;
      environment.PATH = `${dirname(inherited.BK_LOADER_BIN)}:${environment.PATH ?? ""}`;
    }
    return markPaths(environment, baseline);
  }
  async describe(sdk: Sdk) {
    const item = await this.record(sdk);
    const sourceExists = item && await exists(join(item.sdk, sdk.requiredFile));
    const toolsExist = item && await exists(item.tools) &&
      (sdk.kind !== "idf" || item.python && await exists(item.python));
    return {
      name: sdkName(sdk), repository: sdk.repository,
      kind: sdk.kind, version: sdk.version, commit: sdk.commit,
      status: !item ? "not installed" : !sourceExists ? "SDK missing"
        : !item.ready ? "sources only" : !toolsExist ? "tools missing" : "installed",
      sdk: resolve(item?.sdk ?? this.sourcePath(sdk), sdk.sdkSubdirectory ?? "."),
      tools: item?.tools ?? this.toolsPath(sdk),
      python: item?.python ?? null,
      path: sdk.kind === "idf"
        ? [item?.python && dirname(item.python), item && join(item.sdk, "tools")].filter(Boolean)
        : usesArm(sdk) && item ? [join(item.tools, "bin")] : [],
      loader: sdk.kind === "bk"
        ? (await this.resources.describe(this.resources.select("bk-loader"))).executable ?? process.env.BK_LOADER_BIN ?? null
        : undefined,
    };
  }
  async defaultEnvironment(inherited: Env = process.env): Promise<Env> {
    const installed: Sdk[] = [];
    for (const sdk of this.entries.filter(item => item.default)) {
      if ((await this.record(sdk))?.ready) installed.push(sdk);
    }
    const hasResources = (await Promise.all(this.resources.entries.map(tool => this.resources.record(tool)))).some(Boolean);
    if (!installed.length && !hasResources) return { ...inherited, GIZOS_DEVENV_HOME: this.root };
    const baseline = cleanEnvironment(inherited);
    const environment = await this.resources.environment(baseline);
    environment.GIZOS_DEVENV_HOME = this.root;
    const hostPaths = (environment.PATH ?? "").split(":");
    const paths: string[] = [];
    for (const sdk of installed) {
      const selected = await this.environment(sdk, undefined, inherited);
      if (selected.GIZOS_BK_TOOLCHAIN_ROOT) {
        environment.GIZOS_BK_TOOLCHAIN_ROOT = selected.GIZOS_BK_TOOLCHAIN_ROOT;
      }
      if (selected.BK_LOADER_BIN) environment.BK_LOADER_BIN = selected.BK_LOADER_BIN;
      for (const key of sdkVariables[sdk.kind]) {
        if (selected[key] !== undefined) environment[key] = selected[key];
      }
      paths.push(...(selected.PATH ?? "").split(":").filter(path => !hostPaths.includes(path)));
    }
    environment.PATH = [...new Set([...paths, ...hostPaths].filter(Boolean))].join(":");
    return markPaths(environment, baseline);
  }
  async toolEnvironment(tool: Tool, inherited: Env = process.env): Promise<Env> {
    const baseline = cleanEnvironment(inherited);
    const environment = await this.resources.environment(await this.defaultEnvironment(baseline), tool);
    return markPaths(environment, baseline);
  }
}


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
