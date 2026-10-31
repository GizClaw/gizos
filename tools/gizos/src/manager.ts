import {
  mkdir, readFile, readdir, rename, rm,
} from "node:fs/promises";
import { existsSync, readFileSync } from "node:fs";
import { homedir } from "node:os";
import { basename, dirname, isAbsolute, join, resolve, sep } from "node:path";
import { createHash, randomUUID } from "node:crypto";
import bundledCatalog from "./catalog.json";
import { Resources, resourceVariables, tools, type Tool } from "./resources";
import {
  atomicJson, downloadArchive, execute, executable, exists, installationPath, locked, quote,
  type Command, type Env, type InstallationPaths, type PathConfiguration,
} from "./common";
import { applyCleanup, cleanupPlan, own, records, selections, selectVersion, withStore } from "./store";
export { execute, exists, locked, type Command } from "./common";

export type Kind = "idf" | "bk" | "ac695n" | "ac707n" | "ac791n";
export type Repository = { alias?: string; id: string; url: string };
export type Sdk = {
  name?: string;
  aliases?: string[];
  chips?: string[];
  sdkVariable?: string;
  armVersion?: string;
  description?: string;
  documentation?: string;
  notes?: string[];
  repositories?: Repository[];
  repositoryAlias?: string;
  repositoryId?: string;
  defaultRepository?: string;
  kind: Kind;
  version: string;
  repository: string;
  commit: string;
  requiredFile: string;
  pythonSeries?: string;
  targets?: string[];
  default?: boolean;
  tags?: string[];
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
  armVersion?: string;
  python?: string;
  ready: boolean;
  targets: string[];
  dependencies?: string[];
};
export type Options = {
  sourcesOnly?: boolean;
  targets?: string[];
  sdk?: string;
  tools?: string;
  python?: string;
  postbuild?: string;
};
type Catalog = Omit<typeof bundledCatalog, "sdks"> & { sdks: Sdk[] };
const definitions: {
  name: string; aliases: string[]; chips?: string[]; sdkVariable?: string; armVersion?: string;
  description?: string; documentation?: string; notes?: string[]; repositories: Repository[];
}[] = bundledCatalog.sdkRepositories;
export const catalog = {
  ...bundledCatalog,
  sdks: bundledCatalog.sdks.map(sdk => {
    const definition = definitions.find(entry => entry.name === sdk.name);
    if (!definition || !definition.repositories.some(repository => repository.url === sdk.repository)) {
      throw new Error(`Missing SDK repository catalog for ${sdk.name}`);
    }
    return { ...definition, ...sdk };
  }),
} as Catalog;
export const PACKAGE = "@gizclaw/gizos";
export const REGISTRY = "https://static-volc.gizclaw.com/npm/";
const controlled = [
  "IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH", "IDF_DEACTIVATE_FILE_PATH", "H2LOADER_IDF_PATH",
  "ESP_IDF_VERSION", "ESP_ROM_ELF_DIR", "OPENOCD_SCRIPTS",
  "BK7258_PATH", "BK_SDK_DIR", "COMPILER_TOOLCHAIN_PATH", "GIZOS_BK_TOOLCHAIN_ROOT",
  ...catalog.sdks.flatMap(sdk => sdk.sdkVariable ? [sdk.sdkVariable] : []),
  ...resourceVariables, "VIRTUAL_ENV", "GIZOS_ENV_PATHS",
  "H2LOADER_PYTHON", "H2LOADER_BOOTSTRAP_PYTHON", "BK3633_SDK_PATH", "BK3633_BINCONVERTER_BIN",
];
const sdkVariables: Record<string, string[]> = {
  idf: ["IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH", "H2LOADER_IDF_PATH", "ESP_IDF_VERSION", "ESP_ROM_ELF_DIR", "OPENOCD_SCRIPTS", "VIRTUAL_ENV"],
  bk: ["BK_SDK_DIR", "COMPILER_TOOLCHAIN_PATH", "GIZOS_BK_TOOLCHAIN_ROOT"],
  ac695n: ["JIELI_AC695N_SDK_PATH"],
  ac707n: ["JIELI_AC707N_SDK_PATH"],
  ac791n: ["JIELI_AC791N_SDK_PATH"],
};

function usesArm(sdk: Sdk): boolean { return sdk.kind === "bk"; }

type ArmToolchain = {
  version: string; compilerVersion: string; hosts: Record<string, { url: string; sha256: string }>;
};
function armToolchain(sdk?: Sdk): ArmToolchain {
  const version = sdk?.armVersion ?? catalog.arm.version;
  const toolchain = ([catalog.arm, ...catalog.armToolchains] as ArmToolchain[])
    .find(entry => entry.version === version);
  if (!toolchain) throw new Error(`Unsupported GNU Arm version ${version}`);
  return toolchain;
}
function armDistribution(sdk?: Sdk) {
  const toolchain = armToolchain(sdk);
  const host = `${process.platform}-${process.arch}`;
  const distribution = toolchain.hosts[host] ??
    (process.platform === "darwin" || process.platform === "linux" && process.arch === "x64"
      ? toolchain.hosts[process.platform] : undefined);
  if (!distribution) throw new Error(`GNU Arm ${toolchain.version} is not available for ${host}`);
  return distribution;
}

function environmentVariables(sdk: Sdk): string[] {
  return sdk.kind === "bk"
    ? [sdk.sdkVariable ?? "BK7258_PATH", ...sdkVariables.bk] : sdkVariables[sdk.kind];
}

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
  const name = sdk.name ?? basename(sdk.repository.replace(/\/$/, "").replace(/\.git$/, ""));
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name)) throw new Error("Invalid SDK name");
  return name;
}

export function sdkChoices(name?: string, entries: Sdk[] = catalog.sdks): Sdk[] {
  if (!name) return entries;
  const named = entries.filter(sdk => sdkName(sdk).toLowerCase() === name.toLowerCase());
  if (named.length) return named;
  const aliased = entries.filter(sdk =>
    [...sdk.aliases ?? [], ...sdk.chips ?? []].some(alias => alias.toLowerCase() === name.toLowerCase()));
  return aliased.length ? aliased : entries.filter(sdk => sdk.kind === kind(name));
}

export function selectSdk(name: string, version: string, entries: Sdk[] = catalog.sdks): Sdk {
  const choices = sdkChoices(name, entries);
  const exact = choices.filter(item => item.version === version || item.tags?.includes(version));
  const matches = exact.length ? exact : /^[0-9a-f]{7,40}$/i.test(version)
    ? choices.filter(item => item.commit.startsWith(version.toLowerCase())) : [];
  if (matches.length > 1) {
    throw new Error(`Ambiguous SDK ${name} ${version}; use an SDK name: ${matches.map(sdkName).join(", ")}`);
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

type Configuration = {
  schema?: 1;
  root?: string;
  repositories?: Record<string, string>;
  paths?: PathConfiguration;
};

const pathKeys = ["sdk", "tools", "python", "path"];
function validPaths(value: unknown): value is InstallationPaths {
  return Boolean(value && typeof value === "object" && !Array.isArray(value) &&
    Object.entries(value).every(([key, path]) =>
      pathKeys.includes(key) && typeof path === "string" && isAbsolute(path) && !/[\0\r\n]/.test(path)));
}

function repositoryChoices(sdk: Sdk): Repository[] {
  return sdk.repositories ?? [{ id: sdkName(sdk), url: sdk.defaultRepository ?? sdk.repository }];
}

function selectRepository(sdk: Sdk, value?: string, stored = false): Repository {
  const choices = repositoryChoices(sdk);
  const repository = value === undefined
    ? choices.find(choice => choice.url === (sdk.defaultRepository ?? sdk.repository))
    : choices.find(choice => [choice.alias, choice.id].some(alias => alias?.toLowerCase() === value.toLowerCase()) ||
      stored && choice.url === value);
  if (!repository) {
    throw new Error(`Repository is not in the catalog for ${sdkName(sdk)}; run gizos list ${sdkName(sdk)} to see available repositories`);
  }
  repositoryAddress(repository.url);
  return repository;
}

function configuredRepository(sdk: Sdk, settings: Configuration): string | undefined {
  for (const name of [sdkName(sdk), ...sdk.aliases ?? []]) {
    if (settings.repositories?.[name] !== undefined) return settings.repositories[name];
  }
}

function repositoryAddress(value: string): string {
  const invalid = () => new Error("Invalid SDK repository address; use an HTTPS/SSH URL or an absolute local path without embedded credentials");
  if (!value || /[\0\r\n]/.test(value)) throw invalid();
  if (isAbsolute(value)) return value;
  if (/^[^\s@:/]+@[^\s:/]+:[^\s]+$/.test(value)) return value;
  if (value !== value.trim() || !/^(https?|ssh|file):\/\//.test(value)) throw invalid();
  try {
    const url = new URL(value);
    if (!["https:", "http:", "ssh:", "file:"].includes(url.protocol) ||
        url.password || url.search || url.hash || (url.protocol !== "ssh:" && url.username) ||
        (url.protocol !== "file:" && !url.hostname) || url.pathname === "/") throw invalid();
    return value;
  } catch { throw invalid(); }
}

export async function configuration(env: Env = process.env): Promise<Configuration> {
  try {
    const value = JSON.parse(await readFile(configFile(env), "utf8"));
    if (!value || typeof value !== "object" || Array.isArray(value) ||
        (value.schema !== undefined && value.schema !== 1) ||
        (value.root !== undefined && (typeof value.root !== "string" || !isAbsolute(value.root))) ||
        (value.repositories !== undefined && (!value.repositories || typeof value.repositories !== "object" ||
          Array.isArray(value.repositories)))) throw new Error("Invalid gizos configuration");
    for (const repository of Object.values(value.repositories ?? {})) {
      if (typeof repository !== "string" || !repository || /[\0\r\n]/.test(repository)) {
        throw new Error("Invalid SDK repository configuration");
      }
    }
    if (value.paths !== undefined) {
      if (!value.paths || typeof value.paths !== "object" || Array.isArray(value.paths) ||
          !Object.values(value.paths).every(versions =>
            versions && typeof versions === "object" && !Array.isArray(versions) &&
            Object.values(versions).every(validPaths))) throw new Error("Invalid installation path configuration");
    }
    return value;
  } catch (error: any) {
    if (error.code !== "ENOENT") throw error;
    return {};
  }
}

async function updateConfiguration(
  update: (value: Configuration) => Configuration, env: Env,
): Promise<Configuration> {
  const file = configFile(env);
  return locked(dirname(file), "configuration", async () => {
    const value = update(await configuration(env));
    await atomicJson(file, { ...value, schema: 1 });
    return value;
  });
}

export async function pathConfiguration(
  name: string, version?: string, changes?: InstallationPaths | null, env: Env = process.env,
  entries: Sdk[] = catalog.sdks, resources: Tool[] = tools,
) {
  if (!version) {
    const key = resources.some(entry => entry.kind === name) ? name
      : [...new Set(sdkChoices(name, entries).map(sdkName))];
    if (typeof key === "string" || key.length === 1) {
      version = (await selections(await dataRoot(undefined, env)))[typeof key === "string" ? key : key[0]];
    }
  }
  const tool = resources.find(entry => entry.kind === name &&
    (version ? entry.version === version || entry.release?.tag === version || entry.release?.commit === version : entry.default));
  const sdk = resources.some(entry => entry.kind === name) ? undefined
    : version ? selectSdk(name, version, entries) : (() => {
      const choices = sdkChoices(name, entries);
      if (new Set(choices.map(sdkName)).size !== 1) throw new Error("Select an SDK and catalog version");
      return choices.find(entry => entry.default) ?? choices[0];
    })();
  if (!sdk && !tool) throw new Error(`Unsupported ${name} version ${version ?? "default"}; run gizos list ${name}`);
  const key = sdk ? sdkName(sdk) : tool!.kind;
  const selectedVersion = (sdk ?? tool)!.version;
  let paths: InstallationPaths | null | undefined = changes;
  if (changes) {
    const allowed = sdk ? ["sdk", "tools", ...(sdk.kind === "idf" ? ["python"] : [])] : ["path"];
    if (!Object.keys(changes).length || Object.keys(changes).some(key => !allowed.includes(key)) ||
        Object.values(changes).some(path => typeof path !== "string" || !path || /[\0\r\n]/.test(path))) {
      throw new Error(`Invalid installation paths for ${key}; use ${allowed.map(key => `--${key}`).join(", ")}`);
    }
    paths = Object.fromEntries(Object.entries(changes).map(([key, path]) => [key, resolve(path)]));
  }
  const settings = paths === undefined ? await configuration(env) : await updateConfiguration(value => {
    const configured = { ...value.paths };
    const versions = { ...configured[key] };
    if (paths === null) delete versions[selectedVersion];
    else versions[selectedVersion] = { ...versions[selectedVersion], ...paths };
    if (Object.keys(versions).length) configured[key] = versions;
    else delete configured[key];
    return { ...value, paths: configured };
  }, env);
  return { name: key, version: selectedVersion, paths: settings.paths?.[key]?.[selectedVersion] ?? {} };
}

export async function sdkConfiguration(
  name: string, repository?: string | null, env: Env = process.env, entries: Sdk[] = catalog.sdks,
) {
  const choices = sdkChoices(name, entries);
  const names = [...new Set(choices.map(sdkName))];
  if (names.length !== 1) throw new Error(`Select an SDK: ${names.join(", ")}`);
  const sdk = choices.find(entry => entry.default) ?? choices[0];
  const key = names[0];
  const defaultRepository = selectRepository(sdk);
  const selected = repository === undefined || repository === null ? repository : selectRepository(sdk, repository);
  const settings = selected === undefined ? await configuration(env) : await updateConfiguration(value => {
    const repositories = { ...value.repositories };
    // A legacy repository name can now cover more than one chip entry.
    for (const other of entries) {
      const otherKey = sdkName(other);
      if (otherKey === key || repositories[otherKey] !== undefined ||
          !other.aliases?.some(alias => sdk.aliases?.includes(alias))) continue;
      const inherited = configuredRepository(other, value);
      if (inherited === undefined) continue;
      try {
        const choice = selectRepository(other, inherited, true);
        repositories[otherKey] = choice.alias ?? choice.id;
      } catch {}
    }
    for (const name of [key, ...sdk.aliases ?? []]) delete repositories[name];
    if (selected !== null) repositories[key] = selected.alias ?? selected.id;
    return { ...value, repositories };
  }, env);
  const configured = configuredRepository(sdk, settings);
  const effective = selectRepository(sdk, configured, true);
  return {
    name: key, chips: sdk.chips ?? [], repositoryAlias: effective.alias ?? effective.id,
    repositoryId: effective.id, repository: effective.url,
    defaultRepositoryAlias: defaultRepository.alias ?? defaultRepository.id,
    defaultRepositoryId: defaultRepository.id, defaultRepository: defaultRepository.url,
    configured: configured !== undefined,
    repositories: repositoryChoices(sdk).map(choice => ({
      ...choice, alias: choice.alias ?? choice.id,
      default: choice.id === defaultRepository.id, selected: choice.id === effective.id,
    })),
  };
}

export async function describeSdk(name: string, env: Env = process.env, entries: Sdk[] = catalog.sdks) {
  const selected = await sdkConfiguration(name, undefined, env, entries);
  const choices = sdkChoices(selected.name, entries);
  const sdk = choices.find(entry => entry.default) ?? choices[0];
  return {
    ...selected,
    description: sdk.description ?? "A pinned SDK managed by the GizOS toolbox.",
    documentation: sdk.documentation ?? null,
    compiler: usesArm(sdk) ? {
      version: armToolchain(sdk).compilerVersion, release: armToolchain(sdk).version,
    } : null,
    chips: [...new Set(choices.flatMap(entry => entry.chips ?? entry.targets ?? []))],
    versions: choices.map(entry => ({
      version: entry.version, default: Boolean(entry.default), commit: entry.commit,
      tags: entry.tags ?? [],
      chips: entry.chips ?? entry.targets ?? [],
    })),
    notes: [
      ...sdk.notes ?? [],
      "SDKs and host tools are shared across worktrees. Selecting a source preserves the catalog's exact commit pins.",
      "Configure independent paths per SDK version with config <sdk> <version> --sdk <checkout> --tools <directory>.",
      "describe, list and config do not download SDKs. Use setup with an explicit catalog version to install an environment.",
    ],
    commands: [
      `gizos config ${selected.name} ${selected.repositoryAlias}`,
      `gizos setup ${selected.name} ${sdk.version}`,
      `gizos env ${selected.name}`,
      `gizos run ${selected.name} ${sdk.version} -- <command>`,
    ],
  };
}

export async function dataRoot(explicit?: string, env: Env = process.env): Promise<string> {
  if (explicit || env.GIZOS_DEVENV_HOME) return resolve(explicit || env.GIZOS_DEVENV_HOME!);
  return (await configuration(env)).root ?? defaultRoot(env);
}

export async function configureRoot(value: string, env: Env = process.env): Promise<string> {
  const root = resolve(value);
  await updateConfiguration(settings => ({ ...settings, root }), env);
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
    public locations: PathConfiguration = {},
  ) {}

  static async configured(
    root: string, command: Command = execute, entries: Sdk[] = catalog.sdks, env: Env = process.env,
  ): Promise<Manager> {
    const settings = await configuration(env);
    return new Manager(root, command, entries.map(sdk => {
      const defaultRepository = sdk.defaultRepository ?? sdk.repository;
      const repository = selectRepository(sdk, configuredRepository(sdk, settings), true);
      return {
        ...sdk, name: sdkName(sdk), defaultRepository,
        repository: repository.url, repositoryId: repository.id, repositoryAlias: repository.alias ?? repository.id,
      };
    }), settings.paths);
  }

  get resources(): Resources {
    return new Resources(this.root, this.command, tools, undefined, this.locations);
  }
  sdk(name: string, version: string): Sdk { return selectSdk(name, version, this.entries); }
  sdks(name?: string): Sdk[] { return sdkChoices(name, this.entries); }
  async currentSdk(name: string): Promise<Sdk> {
    const choices = this.sdks(name);
    const names = [...new Set(choices.map(sdkName))];
    if (names.length !== 1) throw new Error(`Select an SDK: ${names.join(", ")}`);
    const version = (await selections(this.root))[names[0]];
    return version ? this.sdk(names[0], version) : choices.find(sdk => sdk.default) ?? choices[0];
  }
  configuredPaths(sdk: Sdk): InstallationPaths { return this.locations[sdkName(sdk)]?.[sdk.version] ?? {}; }
  sourcePath(sdk: Sdk): string {
    const configured = this.configuredPaths(sdk).sdk;
    if (configured) return configured;
    const paths = [sdkName(sdk), ...sdk.aliases ?? []]
      .map(name => join(this.root, "sdks", name, `${sdk.version}-${sdk.commit}`));
    return paths.find(path => existsSync(path)) ?? paths[0];
  }
  poolFile(sdk: Sdk): string {
    const version = usesArm(sdk) ? `${armToolchain(sdk).version}-` : "";
    const family = usesArm(sdk) ? "bk" : sdk.kind;
    return join(this.root, "pools", `${family}-${version}${process.platform}-${process.arch}.json`);
  }
  toolsPath(sdk: Sdk): string {
    const configured = this.configuredPaths(sdk).tools;
    if (configured) return configured;
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
      : join(this.root, "tools", "arm", `${armToolchain(sdk).version}-${process.platform}-${process.arch}`);
  }
  statePath(sdk: Sdk): string {
    return join(this.root, "environments", sdkName(sdk), `${sdk.version}.json`);
  }
  async record(sdk: Sdk): Promise<Installation | undefined> {
    const names = [sdkName(sdk), ...sdk.aliases ?? []];
    const paths = [...new Set([
      ...names.map(name => join(this.root, "environments", name, `${sdk.version}.json`)),
      join(this.root, "environments", sdk.kind, `${sdk.version}.json`),
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
            (item.name !== undefined && !names.includes(item.name)) ||
            (item.repository !== undefined && typeof item.repository !== "string") ||
            (item.armVersion !== undefined && typeof item.armVersion !== "string") ||
            (item.python !== undefined && !isAbsolute(item.python))) {
          throw new Error(`Invalid installation record: ${path}`);
        }
        if (item.repository !== undefined) repositoryAddress(item.repository);
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
      await own(this.root, path, "sdk", sdk.commit);
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
  async verifyArm(tools: string, sdk?: Sdk): Promise<void> {
    const compiler = join(tools, "bin", "arm-none-eabi-gcc");
    await executable(compiler);
    const version = await this.command([compiler, "-dumpfullversion", "-dumpversion"]);
    if (version.trim() !== armToolchain(sdk).compilerVersion) {
      throw new Error(`GNU Arm compiler mismatch: ${compiler}`);
    }
    for (const tool of ["arm-none-eabi-gcc-ar", "arm-none-eabi-objcopy"]) {
      await executable(join(tools, "bin", tool));
    }
  }
  async downloadArm(url: string, partial: string): Promise<void> {
    await downloadArchive(url, partial, this.command);
  }
  async installArm(tools: string, sdk?: Sdk): Promise<void> {
    if (await exists(tools)) return this.verifyArm(tools, sdk);
    const distribution = armDistribution(sdk);
    const cache = join(this.root, "downloads");
    await mkdir(cache, { recursive: true });
    const archive = join(cache, `${distribution.sha256}-${basename(distribution.url)}`);
    const digest = async (path: string) => {
      const hash = createHash("sha256");
      for await (const chunk of Bun.file(path).stream()) hash.update(chunk);
      return hash.digest("hex");
    };
    if (!await exists(archive)) {
      console.error(`Downloading GNU Arm ${armToolchain(sdk).version}...`);
      const partial = `${archive}.${randomUUID()}.tmp`;
      try {
        await this.downloadArm(distribution.url, partial);
        if (await digest(partial) !== distribution.sha256) throw new Error("GNU Arm archive checksum mismatch");
        await rename(partial, archive);
        await own(this.root, archive, "download");
      } finally { await rm(partial, { force: true }); }
    }
    if (await digest(archive) !== distribution.sha256) throw new Error(`Corrupt cached archive: ${archive}`);
    const temporary = `${tools}.${randomUUID()}.tmp`;
    await mkdir(temporary, { recursive: true });
    try {
      await this.command(["tar", "-xf", archive, "--strip-components=1", "-C", temporary]);
      await this.verifyArm(temporary, sdk);
      await rename(temporary, tools);
      await own(this.root, tools, "tools");
    } finally { await rm(temporary, { recursive: true, force: true }); }
  }
  async setup(sdk: Sdk, options: Options = {}, adopt = false): Promise<Installation> {
    if (!["darwin", "linux"].includes(process.platform)) throw new Error("SDK setup currently supports macOS and Linux");
    return withStore(this.root, () => locked(this.root, `${sdkName(sdk)}-${sdk.version}`, async () => {
      const previous = await this.record(sdk);
      const locations = this.configuredPaths(sdk);
      const requested = {
        sdk: options.sdk ?? locations.sdk, tools: options.tools ?? locations.tools,
        python: options.python ?? locations.python,
      };
      const armVersion = usesArm(sdk) ? armToolchain(sdk).version : undefined;
      const sameTools = !armVersion || !previous || (previous.armVersion ?? catalog.arm.version) === armVersion;
      const sdkPath = requested.sdk ? await installationPath(requested.sdk) : previous?.sdk ?? this.sourcePath(sdk);
      const toolsPath = requested.tools ? await installationPath(requested.tools)
        : sameTools ? previous?.tools ?? this.toolsPath(sdk) : this.toolsPath(sdk);
      const item: Installation = {
        schema: 1, name: sdkName(sdk),
        repository: previous?.sdk === sdkPath ? previous.repository : sdk.repository,
        kind: sdk.kind, version: sdk.version, commit: sdk.commit,
        sdk: sdkPath, tools: toolsPath,
        armVersion,
        // A virtualenv's Python can be a symlink to the host interpreter.
        python: requested.python ? resolve(requested.python) : previous?.tools === toolsPath ? previous.python : undefined,
        ready: Boolean(previous?.ready && sameTools && previous.sdk === sdkPath && previous.tools === toolsPath),
        targets: options.targets ?? previous?.targets ?? sdk.targets ?? [],
        dependencies: previous?.dependencies,
      };
      if (usesArm(sdk) && !adopt && !options.sourcesOnly && !await exists(item.tools)) armDistribution(sdk);
      if (adopt) await this.verifySource(sdk, item.sdk);
      else await this.fetchSource(sdk, item.sdk);
      const dependenciesReady = !usesArm(sdk) ||
        await this.resources.record(await this.resources.current("bk-loader"));
      const reuse = item.ready && dependenciesReady && !options.sourcesOnly && !adopt &&
        await exists(item.tools) && (sdk.kind !== "idf" || item.python && await exists(item.python)) &&
        item.targets.every(target => previous?.targets.includes(target)) &&
        (!requested.python || requested.python === previous?.python);
      if (reuse) {
        await this.environment(sdk, item);
        await selectVersion(this.root, sdkName(sdk), sdk.version);
        return item;
      }
      if (!options.sourcesOnly) {
        // IDF owns the family/version layout within its default shared tools root.
        await locked(this.root, `${usesArm(sdk) ? "bk" : sdk.kind}-tools`, async () => {
          if (!requested.tools && !previous?.ready) item.tools = this.toolsPath(sdk);
          if (sdk.kind === "idf") {
            if (!adopt) {
              const newTools = !await exists(item.tools);
              const priorPython = new Set(await readdir(join(item.tools, "python_env")).catch((error: any) => {
                if (error.code !== "ENOENT") throw error;
                return [];
              }));
              const environment = cleanEnvironment();
              environment.IDF_TOOLS_PATH = item.tools;
              if (item.python) environment.IDF_PYTHON_ENV_PATH = dirname(dirname(item.python));
              await this.command(["bash", join(item.sdk, "install.sh"), item.targets.join(",")], {
                cwd: item.sdk, env: environment, inherit: true,
              });
              if (newTools) await own(this.root, item.tools, "tools");
              for (const name of await readdir(join(item.tools, "python_env")).catch((error: any) => {
                if (error.code !== "ENOENT") throw error;
                return [];
              })) {
                if (!priorPython.has(name)) await own(this.root, join(item.tools, "python_env", name), "python");
              }
            }
            item.python = item.python ?? await this.findPython(sdk, item.tools);
            await executable(item.python);
          } else if (usesArm(sdk)) {
            if (adopt) await this.verifyArm(item.tools, sdk);
            else await this.installArm(item.tools, sdk);
            if (!adopt) {
              const loader = await this.resources.current("bk-loader");
              const installed = await this.resources.setup(loader, undefined, undefined, false);
              item.dependencies = [installed.root];
            }
          } else {
            const toolchain = this.resources.select("jieli-toolchain");
            const postbuild = this.resources.select("jieli-postbuild");
            const existing = await this.resources.record(toolchain);
            const existingPostbuild = await this.resources.record(postbuild);
            if (adopt && (!requested.tools && !existing || !options.postbuild && !existingPostbuild)) {
              throw new Error("adopt requires registered JieLi tools or --tools and --postbuild paths");
            }
            item.tools = (await this.resources.setup(
              toolchain, adopt ? requested.tools ?? existing?.root : undefined, requested.tools, false,
            )).root;
            const installed = await this.resources.setup(
              postbuild, options.postbuild ?? (adopt ? existingPostbuild?.root : undefined), undefined, false,
            );
            item.dependencies = [installed.root];
          }
          item.ready = true;
          await this.environment(sdk, item);
          if (!locations.tools && !sdk.kind.startsWith("ac") && !await exists(this.poolFile(sdk))) {
            await atomicJson(this.poolFile(sdk), { schema: 1, tools: item.tools });
          }
        });
      } else if (item.ready) await this.environment(sdk, item);
      await atomicJson(this.statePath(sdk), item);
      if (item.ready) await selectVersion(this.root, sdkName(sdk), sdk.version);
      return item;
    }));
  }
  async remove(name: string, version: string, dryRun = false) {
    const work = async () => {
      const tool = this.resources.isTool(name) ? this.resources.select(name, version) : undefined;
      const sdk = tool ? undefined : this.sdk(name, version);
      const key = tool ? tool.kind : sdkName(sdk!);
      const current = tool ? await this.resources.current(key) : await this.currentSdk(key);
      const selected = tool ? await this.resources.record(tool) : await this.record(sdk!);
      if (!selected) throw new Error(`${key} ${version} is not installed`);
      const all = [...await records(this.root, "environments"), ...await records(this.root, "resources")];
      const files = all.filter(({ path, value }) => tool
        ? path === this.resources.statePath(tool)
        : value.commit === sdk!.commit && value.kind === sdk!.kind && value.version === sdk!.version &&
          (value.name === undefined || [key, ...sdk!.aliases ?? []].includes(value.name)) &&
          value.sdk === (selected as Installation).sdk).map(item => item.path);
      if (tool) {
        const active = await this.resources.current(tool.kind);
        const dependent = all.find(({ path, value }) => !files.includes(path) &&
          (value.dependencies?.includes((selected as { root: string }).root) ||
            !value.dependencies && tool.version === active.version && value.ready &&
            (value.kind === "bk" && tool.kind === "bk-loader" ||
              value.kind?.startsWith("ac") && ["jieli-toolchain", "jieli-postbuild"].includes(tool.kind))));
        if (dependent) throw new Error(`${key} ${version} is required by ${dependent.value.name ?? dependent.value.kind} ${dependent.value.version}`);
      }
      const item = selected as Installation;
      const candidates = tool ? [(selected as { root: string }).root] : [
        item.sdk, ...(item.python ? [dirname(dirname(item.python))] : []),
      ];
      const plan = await cleanupPlan(this.root, files, await Promise.all(candidates.map(installationPath)), this.command);
      if (!dryRun) {
        for (const path of files) await rm(path, { force: true });
        if (current.version === (sdk ?? tool)!.version) {
          const alternatives = tool ? this.resources.entries.filter(entry => entry.kind === key && entry.version !== tool.version)
            : this.sdks(key).filter(entry => entry.version !== sdk!.version);
          const installed: (Sdk | Tool)[] = [];
          for (const candidate of alternatives) {
            const record = tool ? await this.resources.record(candidate as Tool) : await this.record(candidate as Sdk);
            if (record && (tool || (record as Installation).ready)) installed.push(candidate);
          }
          await selectVersion(this.root, key, (installed.find(entry => entry.default) ?? installed[0])?.version);
        }
        await applyCleanup(this.root, plan);
      }
      return { name: key, version: (sdk ?? tool)!.version, dryRun, records: files, ...plan };
    };
    return dryRun ? work() : withStore(this.root, work);
  }
  async prune(dryRun = false) {
    const work = async () => {
      const plan = await cleanupPlan(this.root, [], undefined, this.command);
      if (!dryRun && await exists(this.root)) await applyCleanup(this.root, plan);
      return { dryRun, ...plan };
    };
    return dryRun || !await exists(this.root) ? work() : withStore(this.root, work);
  }
  async environment(sdk: Sdk, selected?: Installation, inherited: Env = process.env): Promise<Env> {
    const item = selected ?? await this.record(sdk);
    if (!item?.ready) throw new Error(`${sdkName(sdk)} ${sdk.version} is not installed; run gizos setup ${sdkName(sdk)} ${sdk.version}`);
    await this.verifySource(sdk, item.sdk);
    const baseline = cleanEnvironment(inherited);
    const environment = await this.resources.environment(baseline);
    environment.GIZOS_DEVENV_HOME = this.root;
    if (usesArm(sdk)) {
      await this.verifyArm(item.tools, sdk);
      Object.assign(environment, {
        COMPILER_TOOLCHAIN_PATH: join(item.tools, "bin"),
        GIZOS_BK_TOOLCHAIN_ROOT: item.tools,
        PATH: `${join(item.tools, "bin")}:${environment.PATH ?? ""}`,
      });
      environment[sdk.sdkVariable ?? "BK7258_PATH"] = item.sdk;
      environment.BK_SDK_DIR = item.sdk;
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
    const configuredPaths = this.configuredPaths(sdk);
    const pendingPaths: InstallationPaths = {};
    if (item) {
      for (const key of ["sdk", "tools", "python"] as const) {
        const configured = configuredPaths[key];
        if (!configured) continue;
        const matches = item[key] && (key === "python" ? configured === item[key]
          : await installationPath(configured) === await installationPath(item[key]!));
        if (!matches) pendingPaths[key] = configured;
      }
    }
    const sourceExists = item && await exists(join(item.sdk, sdk.requiredFile));
    const toolsExist = item && await exists(item.tools) &&
      (!usesArm(sdk) || (item.armVersion ?? catalog.arm.version) === armToolchain(sdk).version) &&
      (sdk.kind !== "idf" || item.python && await exists(item.python));
    return {
      name: sdkName(sdk), chips: sdk.chips ?? sdk.targets ?? [],
      repositoryAlias: sdk.repositoryAlias ?? selectRepository(sdk).alias ?? selectRepository(sdk).id,
      repositoryId: sdk.repositoryId ?? selectRepository(sdk).id, repository: sdk.repository,
      defaultRepository: sdk.defaultRepository ?? sdk.repository,
      installedRepository: item?.repository ?? null,
      kind: sdk.kind, version: sdk.version, commit: sdk.commit,
      selected: (await this.currentSdk(sdkName(sdk))).version === sdk.version,
      status: !item ? "not installed" : !sourceExists ? "SDK missing"
        : !item.ready ? "sources only" : !toolsExist ? "tools missing" : "installed",
      sdk: resolve(item?.sdk ?? this.sourcePath(sdk), sdk.sdkSubdirectory ?? "."),
      tools: item?.tools ?? this.toolsPath(sdk),
      configuredPaths, pendingPaths,
      python: item?.python ?? null,
      path: sdk.kind === "idf"
        ? [item?.python && dirname(item.python), item && join(item.sdk, "tools")].filter(Boolean)
        : usesArm(sdk) && item ? [join(item.tools, "bin")] : [],
      loader: sdk.kind === "bk"
        ? (await this.resources.describe(await this.resources.current("bk-loader"))).executable ?? process.env.BK_LOADER_BIN ?? null
        : undefined,
    };
  }
  async defaultEnvironment(inherited: Env = process.env): Promise<Env> {
    const installed: Sdk[] = [];
    for (const name of [...new Set(this.entries.map(sdkName))]) {
      const sdk = await this.currentSdk(name);
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
      if (selected.GIZOS_BK_TOOLCHAIN_ROOT &&
          (!environment.GIZOS_BK_TOOLCHAIN_ROOT || sdk.kind === "bk" && !environment.BK_SDK_DIR)) {
        environment.GIZOS_BK_TOOLCHAIN_ROOT = selected.GIZOS_BK_TOOLCHAIN_ROOT;
      }
      if (selected.BK_LOADER_BIN) environment.BK_LOADER_BIN = selected.BK_LOADER_BIN;
      // Export each named BK locator while keeping the first installed BK SDK as the generic default.
      for (const key of environmentVariables(sdk)) {
        if (selected[key] !== undefined && (sdk.kind !== "bk" || environment[key] === undefined)) {
          environment[key] = selected[key];
        }
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
