import { chmod, mkdir, readFile, realpath, rename, rm, writeFile } from "node:fs/promises";
import { dirname, isAbsolute, join, resolve } from "node:path";
import { createHash, randomUUID } from "node:crypto";
import bundledCatalog from "./catalog.json";
import { atomicJson, execute, executable, exists, locked, type Command, type Env } from "./common";

type Source = { repository: string; commit: string; path: string; lfs?: boolean };
type Distribution = { url?: string; source?: Source; sha256?: string; format?: string; strip?: number; installer?: string };
export type Tool = {
  kind: string; version: string; default?: boolean;
  hosts: Record<string, Distribution>; requiredFiles: string[];
  executable?: string; check?: string[]; expectedOutput?: string;
  exports: Record<string, string>; path: string[];
  executableHosts?: string[]; marker?: boolean; rename?: Record<string, string>;
  executableSha256?: string;
};
type ToolRecord = { schema: 1; kind: string; version: string; host: string; root: string };
export const tools = bundledCatalog.tools as unknown as Tool[];
export const resourceVariables = [
  ...new Set(tools.flatMap(tool => Object.keys(tool.exports))),
  "JIELI_AC695N_SDK_PATH", "JIELI_AC707N_SDK_PATH", "JIELI_AC791N_SDK_PATH",
];

export async function sha256(path: string): Promise<string> {
  const hash = createHash("sha256");
  for await (const chunk of Bun.file(path).stream()) hash.update(chunk);
  return hash.digest("hex");
}

export class Resources {
  constructor(
    public root: string, public command: Command = execute, public entries: Tool[] = tools,
    public host = `${process.platform}-${process.arch}`,
  ) {}

  isTool(name: string): boolean { return this.entries.some(tool => tool.kind === name); }
  select(name: string, version?: string): Tool {
    const tool = this.entries.find(item => item.kind === name && (version ? item.version === version : item.default));
    if (!tool) throw new Error(`Unsupported ${name} version ${version ?? "default"}; run gizos list ${name}`);
    return tool;
  }
  distribution(tool: Tool): Distribution | undefined {
    return tool.hosts[this.host] ?? tool.hosts[this.host.split("-")[0]] ?? tool.hosts.any;
  }
  canRun(tool: Tool): boolean {
    return Boolean(this.distribution(tool)) && (!tool.executableHosts || tool.executableHosts.includes(this.host));
  }
  statePath(tool: Tool): string {
    return join(this.root, "resources", tool.kind, `${tool.version}-${this.host}.json`);
  }
  async record(tool: Tool): Promise<ToolRecord | undefined> {
    try {
      const item = JSON.parse(await readFile(this.statePath(tool), "utf8"));
      if (item.schema !== 1 || item.kind !== tool.kind || item.version !== tool.version ||
          item.host !== this.host || typeof item.root !== "string" || !isAbsolute(item.root)) {
        throw new Error(`Invalid tool record: ${this.statePath(tool)}`);
      }
      return item;
    } catch (error: any) {
      if (error.code !== "ENOENT") throw error;
    }
  }
  async verify(tool: Tool, root: string, run = true): Promise<void> {
    for (const path of tool.requiredFiles) {
      if (!await exists(join(root, path))) throw new Error(`${tool.kind} input is missing: ${join(root, path)}`);
      const file = Bun.file(join(root, path));
      if ((await file.slice(0, 64).text().catch(() => "")).startsWith("version https://git-lfs.github.com/spec/v1")) {
        throw new Error(`${tool.kind} contains an unresolved Git LFS pointer: ${path}`);
      }
    }
    if (tool.marker && (await readFile(join(root, ".firmware-devenv-version"), "utf8")).trim() !== tool.version) {
      throw new Error(`${tool.kind} version marker differs from ${tool.version}`);
    }
    if (tool.executable) {
      const entry = join(root, tool.executable);
      await executable(entry);
      if (tool.executableSha256 && await sha256(entry) !== tool.executableSha256) {
        throw new Error(`${tool.kind} executable checksum mismatch: ${entry}`);
      }
      if (run && this.canRun(tool) && tool.check) {
        const output = await this.command([entry, ...tool.check], { cwd: root });
        if (tool.expectedOutput && !output.includes(tool.expectedOutput)) {
          throw new Error(`${tool.kind} version mismatch: expected ${tool.version}`);
        }
      }
    }
  }
  async snapshot(source: Source): Promise<string> {
    if (!/^[a-f0-9]{40}$/.test(source.commit) ||
        source.path !== "." && source.path.split("/").some(part => !part || part === ".." || part === ".")) {
      throw new Error("Invalid pinned vendor asset source");
    }
    const identity = createHash("sha256").update(JSON.stringify(source)).digest("hex");
    const checkout = join(this.root, "assets", identity);
    return locked(this.root, `asset-${identity}`, async () => {
      const git = (directory: string, ...args: string[]) => this.command(
        ["git", "-c", "core.hooksPath=/dev/null", "-C", directory, ...args],
        { env: { ...process.env, GIT_TERMINAL_PROMPT: "0", GIT_LFS_SKIP_SMUDGE: "1" } },
      );
      if (!await exists(checkout)) {
        const temporary = `${checkout}.${randomUUID()}.tmp`;
        await mkdir(temporary, { recursive: true });
        try {
          await git(temporary, "init", "--quiet");
          await git(temporary, "remote", "add", "origin", source.repository);
          if (source.path !== ".") {
            await git(temporary, "sparse-checkout", "set", "--no-cone", `/${source.path}`, `/${source.path}/**`);
          }
          await git(temporary, "fetch", "--depth=1", "origin", source.commit);
          await git(temporary, "checkout", "--quiet", "--detach", "FETCH_HEAD");
          if (source.lfs) {
            await git(temporary, "lfs", "pull", `--include=${source.path},${source.path}/**`, "--exclude=");
          }
          await rename(temporary, checkout);
        } finally { await rm(temporary, { recursive: true, force: true }); }
      }
      if ((await git(checkout, "rev-parse", "HEAD")).trim() !== source.commit ||
          (await git(checkout, "status", "--porcelain=v1", "--untracked-files=no")).trim()) {
        throw new Error(`Vendor assets are changed; existing files were preserved: ${checkout}`);
      }
      return join(checkout, source.path);
    });
  }
  async download(distribution: Distribution): Promise<string> {
    const source = distribution.source ? await this.snapshot(distribution.source) : undefined;
    if (source) {
      if (distribution.sha256 && await sha256(source) !== distribution.sha256) {
        throw new Error("Vendor archive checksum mismatch");
      }
      return source;
    }
    if (!distribution.url || !/^[a-f0-9]{64}$/.test(distribution.sha256 ?? "")) {
      throw new Error("Tool distribution must have a URL and exact SHA-256");
    }
    const archive = join(this.root, "downloads", distribution.sha256!);
    if (!await exists(archive)) {
      await mkdir(dirname(archive), { recursive: true });
      const temporary = `${archive}.${randomUUID()}.tmp`;
      try {
        const response = await fetch(distribution.url);
        if (!response.ok) throw new Error(`Tool download failed: HTTP ${response.status}`);
        await Bun.write(temporary, response);
        if (await sha256(temporary) !== distribution.sha256) throw new Error("Tool archive checksum mismatch");
        await rename(temporary, archive);
      } finally { await rm(temporary, { force: true }); }
    }
    if (await sha256(archive) !== distribution.sha256) throw new Error(`Corrupt cached tool archive: ${archive}`);
    return archive;
  }
  async setup(tool: Tool, existing?: string): Promise<ToolRecord> {
    const distribution = this.distribution(tool);
    if (!distribution) throw new Error(`UNSUPPORTED ${tool.kind} host: ${this.host}`);
    return locked(this.root, `tool-${tool.kind}-${tool.version}-${this.host}`, async () => {
      const previous = await this.record(tool);
      if (existing || previous) {
        const root = existing ? await realpath(resolve(existing)) : previous!.root;
        await this.verify(tool, root);
        const record: ToolRecord = { schema: 1, kind: tool.kind, version: tool.version, host: this.host, root };
        await atomicJson(this.statePath(tool), record);
        return record;
      }
      const path = join(this.root, "tools", tool.kind, `${tool.version}-${this.host}`);
      let installed = path;
      if (!distribution.format && !distribution.installer) {
        if (!distribution.source) throw new Error("Tool has no pinned installation source");
        installed = await this.snapshot(distribution.source);
        await this.verify(tool, installed);
      } else if (await exists(path)) {
        await this.verify(tool, path);
      } else {
        const archive = distribution.installer ? undefined : await this.download(distribution);
        const temporary = `${path}.${randomUUID()}.tmp`;
        await mkdir(temporary, { recursive: true });
        try {
          if (distribution.installer === "python") {
            if (!distribution.source) throw new Error("Python tool installation requires a pinned source");
            const source = await this.snapshot(distribution.source);
            await this.command(["python3", "-m", "venv", temporary]);
            await this.command([
              join(temporary, "bin/python"), "-m", "pip", "install",
              "--disable-pip-version-check", "--no-input", source,
            ], { inherit: true });
            // Console-script shebangs contain the staging path; this launcher
            // locates its Python environment after the atomic directory rename.
            const entry = join(temporary, tool.executable!);
            await writeFile(entry, `#!/bin/sh\nexec "$(dirname -- "$0")/python" -c 'from bk_loader import cli; cli(prog_name="bk_loader")' "$@"\n`);
            await chmod(entry, 0o755);
          } else if (distribution.format === "zip") {
            await this.command(["unzip", "-q", archive!, "-d", temporary]);
          } else {
            await this.command(["tar", "-xf", archive!, "-C", temporary, `--strip-components=${distribution.strip ?? 0}`]);
          }
          for (const [before, after] of Object.entries(tool.rename ?? {})) {
            await rename(join(temporary, before), join(temporary, after));
          }
          if (tool.marker) await writeFile(join(temporary, ".firmware-devenv-version"), tool.version + "\n");
          await this.verify(tool, temporary);
          await rename(temporary, path);
        } finally { await rm(temporary, { recursive: true, force: true }); }
      }
      const record: ToolRecord = { schema: 1, kind: tool.kind, version: tool.version, host: this.host, root: installed };
      await atomicJson(this.statePath(tool), record);
      return record;
    });
  }
  async describe(tool: Tool) {
    const item = await this.record(tool);
    const present = item && (await Promise.all(tool.requiredFiles.map(path => exists(join(item.root, path))))).every(Boolean);
    return {
      kind: tool.kind, version: tool.version,
      status: !this.distribution(tool) ? "unsupported host" : !item ? "not installed" : !present ? "tools missing" : "installed",
      tools: item?.root ?? join(this.root, "tools", tool.kind, `${tool.version}-${this.host}`),
      executable: item && tool.executable ? join(item.root, tool.executable) : null,
      path: item && this.canRun(tool) ? tool.path.map(path => resolve(item.root, path)) : [],
      host: this.host, canRun: this.canRun(tool), source: this.distribution(tool)?.source ?? null,
    };
  }
  async environment(inherited: Env, selected?: Tool): Promise<Env> {
    const environment = { ...inherited };
    const paths: string[] = [];
    for (const tool of selected ? [selected] : this.entries.filter(item => item.default)) {
      const item = await this.record(tool);
      if (!item) {
        if (selected) throw new Error(`${tool.kind} ${tool.version} is not installed; run gizos setup ${tool.kind} ${tool.version}`);
        continue;
      }
      await this.verify(tool, item.root, false);
      if (!this.distribution(tool)) continue;
      // Linux-hosted JieLi trees remain locatable for container builds.
      for (const [key, path] of Object.entries(tool.exports)) {
        environment[key] = resolve(item.root, path);
      }
      if (this.canRun(tool)) paths.push(...tool.path.map(path => resolve(item.root, path)));
    }
    environment.PATH = [...new Set([...paths, ...(environment.PATH ?? "").split(":")].filter(Boolean))].join(":");
    return environment;
  }
}
