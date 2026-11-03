import { chmod, cp, mkdir, mkdtemp, readFile, realpath, rename, rm, writeFile } from "node:fs/promises";
import { dirname, isAbsolute, join, relative, resolve } from "node:path";
import { tmpdir } from "node:os";
import { createHash, randomUUID } from "node:crypto";
import bundledCatalog from "./catalog.json";
import {
  atomicJson, downloadArchive, execute, executable, exists, installationPath, locked,
  type Command, type Env, type PathConfiguration,
} from "./common";
import { own, selections, selectVersion, withStore } from "./store";

type Source = { repository: string; commit: string; path: string; lfs?: boolean };
type Distribution = {
  url?: string; source?: Source; sha256?: string; format?: string; strip?: number; installer?: string;
  executableSha256?: string; subdirectory?: string; requiredFiles?: string[];
  build?: { target: string; output: string };
};
export type Tool = {
  kind: string; version: string; default?: boolean; retired?: boolean;
  release?: { repository: string; tag: string; commit: string };
  upstream?: { version: string; cliVersion: string; releasedAt: string; documentation: string };
  application?: { path: string; identifier: string };
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
    public locations: PathConfiguration = {},
  ) {}

  configuredPath(tool: Tool): string | undefined { return this.locations[tool.kind]?.[tool.version]?.path; }
  isTool(name: string): boolean { return this.entries.some(tool => tool.kind === name); }
  select(name: string, version?: string): Tool {
    const tool = this.entries.find(item => item.kind === name && (version
      ? item.version === version || item.release?.tag === version || item.release?.commit === version
      : item.default));
    if (!tool) throw new Error(`Unsupported ${name} version ${version ?? "default"}; run gizos list ${name}`);
    return tool;
  }
  async current(name: string): Promise<Tool> {
    return this.select(name, (await selections(this.root))[name]);
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
    for (const path of [...tool.requiredFiles, ...this.distribution(tool)?.requiredFiles ?? []]) {
      if (!await exists(join(root, path))) throw new Error(`${tool.kind} input is missing: ${join(root, path)}`);
      const file = Bun.file(join(root, path));
      if ((await file.slice(0, 64).text().catch(() => "")).startsWith("version https://git-lfs.github.com/spec/v1")) {
        throw new Error(`${tool.kind} contains an unresolved Git LFS pointer: ${path}`);
      }
    }
    if (tool.marker && (await readFile(join(root, ".firmware-devenv-version"), "utf8")).trim() !== tool.version) {
      throw new Error(`${tool.kind} version marker differs from ${tool.version}`);
    }
    if (tool.application && this.canRun(tool)) {
      const info = join(root, tool.application.path, "Contents/Info.plist");
      for (const [key, expected] of [
        ["CFBundleIdentifier", tool.application.identifier], ["CFBundleShortVersionString", tool.version],
      ]) {
        const value = await this.command(["/usr/bin/plutil", "-extract", key, "raw", info]);
        if (value.trim() !== expected) throw new Error(`${tool.kind} application ${key} differs from ${expected}`);
      }
    }
    if (tool.executable) {
      const entry = join(root, tool.executable);
      await executable(entry);
      const expected = this.distribution(tool)?.executableSha256 ?? tool.executableSha256;
      if (expected && await sha256(entry) !== expected) {
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
          await own(this.root, checkout, "snapshot", source.commit);
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
        await downloadArchive(distribution.url, temporary, this.command);
        if (await sha256(temporary) !== distribution.sha256) throw new Error("Tool archive checksum mismatch");
        await rename(temporary, archive);
        await own(this.root, archive, "download");
      } finally { await rm(temporary, { force: true }); }
    }
    if (await sha256(archive) !== distribution.sha256) throw new Error(`Corrupt cached tool archive: ${archive}`);
    return archive;
  }
  async importArchive(tool: Tool, path: string): Promise<string> {
    if (tool.retired) throw new Error(`${tool.kind} standalone installation is retired; run gizos install gizloader`);
    const distribution = this.distribution(tool);
    if (!distribution) throw new Error(`UNSUPPORTED ${tool.kind} host: ${this.host}`);
    const expected = distribution.sha256;
    if (!distribution.url || !distribution.format || !/^[a-f0-9]{64}$/.test(expected ?? "")) {
      throw new Error("This tool does not support importing a checksummed archive");
    }
    const source = resolve(path);
    if (await sha256(source) !== expected) throw new Error("Tool archive checksum mismatch");
    return withStore(this.root, () => locked(this.root, `download-${expected}`, async () => {
      const archive = join(this.root, "downloads", expected!);
      if (!await exists(archive)) {
        await mkdir(dirname(archive), { recursive: true });
        const temporary = `${archive}.${randomUUID()}.tmp`;
        try {
          await cp(source, temporary);
          if (await sha256(temporary) !== expected) throw new Error("Tool archive checksum mismatch");
          await rename(temporary, archive);
          await own(this.root, archive, "download");
        } finally { await rm(temporary, { force: true }); }
      }
      if (await sha256(archive) !== expected) throw new Error(`Corrupt cached tool archive: ${archive}`);
      return archive;
    }));
  }
  async unpack(distribution: Distribution, archive: string, destination: string): Promise<void> {
    if (distribution.format === "dmg") {
      if (!distribution.subdirectory?.endsWith(".app")) throw new Error("A DMG tool must select an application bundle");
      // macOS's disk-image helper needs an internal-volume mount point.
      const mount = await mkdtemp(join(process.platform === "darwin" ? "/private/tmp" : tmpdir(), "gizos-dmg-"));
      let mounted = false;
      try {
        await this.command(["/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mount, archive]);
        mounted = true;
        await cp(join(mount, distribution.subdirectory), join(destination, distribution.subdirectory), {
          recursive: true, verbatimSymlinks: true,
        });
      } finally {
        if (mounted) await this.command(["/usr/bin/hdiutil", "detach", mount]);
        await rm(mount, { recursive: true, force: true });
      }
      return;
    }
    const nested = distribution.format === "pkg" || Boolean(distribution.subdirectory);
    const extracted = nested ? `${destination}.extract` : destination;
    try {
      if (distribution.format === "pkg") {
        // Expand the payload without running installer scripts or writing to /usr/local.
        await this.command(["/usr/sbin/pkgutil", "--expand-full", archive, extracted]);
      } else {
        await mkdir(extracted, { recursive: true });
        if (distribution.format === "zip") {
          await this.command(["unzip", "-q", archive, "-d", extracted]);
        } else {
          await this.command(["tar", "-xf", archive, "-C", extracted, `--strip-components=${distribution.strip ?? 0}`]);
        }
      }
      if (nested) await cp(join(extracted, distribution.subdirectory ?? "Payload"), destination, {
        recursive: true, verbatimSymlinks: true,
      });
    } finally {
      if (nested) await rm(extracted, { recursive: true, force: true });
    }
  }
  async setup(tool: Tool, existing?: string, destination?: string, activate = true): Promise<ToolRecord> {
    const distribution = this.distribution(tool);
    if (!distribution) throw new Error(`UNSUPPORTED ${tool.kind} host: ${this.host}`);
    return withStore(this.root, () => locked(this.root, `tool-${tool.kind}-${tool.version}-${this.host}`, async () => {
      const previous = await this.record(tool);
      const configured = destination ?? this.configuredPath(tool);
      if (existing || previous && (!configured || await installationPath(configured) === await installationPath(previous.root))) {
        const root = existing ? await realpath(resolve(existing)) : previous!.root;
        await this.verify(tool, root);
        const record: ToolRecord = { schema: 1, kind: tool.kind, version: tool.version, host: this.host, root };
        await atomicJson(this.statePath(tool), record);
        if (activate) await selectVersion(this.root, tool.kind, tool.version);
        return record;
      }
      if (tool.retired) throw new Error(`${tool.kind} standalone installation is retired; run gizos install gizloader`);
      const path = configured ?? join(this.root, "tools", tool.kind, `${tool.version}-${this.host}`);
      let installed = path;
      if (!distribution.format && !distribution.installer) {
        if (!distribution.source) throw new Error("Tool has no pinned installation source");
        installed = await this.snapshot(distribution.source);
        await this.verify(tool, installed);
        if (configured) {
          if (await exists(path)) await this.verify(tool, path);
          else {
            const temporary = `${path}.${randomUUID()}.tmp`;
            await mkdir(dirname(temporary), { recursive: true });
            try {
              await cp(installed, temporary, { recursive: true, verbatimSymlinks: true });
              await this.verify(tool, temporary);
              await rename(temporary, path);
              await own(this.root, path, "tools");
            } finally { await rm(temporary, { recursive: true, force: true }); }
          }
          installed = path;
        }
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
            await mkdir(dirname(entry), { recursive: true });
            const python = relative(dirname(entry), join(temporary, "bin/python"));
            await writeFile(entry, `#!/bin/sh\nexec "$(dirname -- "$0")/${python}" -c 'from bk_loader import cli; cli(prog_name="bk_loader")' "$@"\n`);
            await chmod(entry, 0o755);
          } else if (distribution.installer === "bazel") {
            if (!distribution.source || !distribution.build || !tool.executable) {
              throw new Error("Native tool installation requires pinned source and a build target");
            }
            const source = await this.snapshot(distribution.source);
            const bazel = process.env.BAZEL_BIN ?? "bazel";
            const config = this.host.startsWith("darwin-") ? "macos" : "linux";
            await this.command([bazel, "build", `--config=${config}`, distribution.build.target], {
              cwd: source, inherit: true,
            });
            const output = (await this.command([bazel, "info", `--config=${config}`, "bazel-bin"], { cwd: source })).trim();
            if (!isAbsolute(output)) throw new Error("Bazel did not report an absolute output directory");
            const entry = join(temporary, tool.executable);
            await mkdir(dirname(entry), { recursive: true });
            await cp(join(output, distribution.build.output), entry);
            await chmod(entry, 0o755);
          } else {
            await this.unpack(distribution, archive!, temporary);
          }
          for (const [before, after] of Object.entries(tool.rename ?? {})) {
            await rename(join(temporary, before), join(temporary, after));
          }
          if (tool.application && tool.executable) {
            const entry = join(temporary, tool.executable);
            const application = relative(dirname(entry), join(temporary, tool.application.path));
            const quote = (value: string) => "'" + value.replaceAll("'", "'\\''") + "'";
            await mkdir(dirname(entry), { recursive: true });
            await writeFile(entry, `#!/bin/sh
case "\${1-}" in
  --version) printf '%s\\n' ${quote(`${tool.kind} ${tool.version}`)}; exit 0 ;;
  --help|-h) printf '%s\\n' ${quote(`Usage: ${tool.kind} [application arguments]`)}; exit 0 ;;
esac
exec /usr/bin/open "$(dirname -- "$0")"/${quote(application)} --args "$@"
`);
            await chmod(entry, 0o755);
          }
          if (tool.marker) await writeFile(join(temporary, ".firmware-devenv-version"), tool.version + "\n");
          await this.verify(tool, temporary);
          await rename(temporary, path);
          await own(this.root, path, "tools");
        } finally { await rm(temporary, { recursive: true, force: true }); }
      }
      const record: ToolRecord = { schema: 1, kind: tool.kind, version: tool.version, host: this.host, root: installed };
      await atomicJson(this.statePath(tool), record);
      if (activate) await selectVersion(this.root, tool.kind, tool.version);
      return record;
    }));
  }
  async describe(tool: Tool) {
    const item = await this.record(tool);
    const configured = this.configuredPath(tool);
    const pending = item && configured && await installationPath(configured) !== await installationPath(item.root);
    const required = [...tool.requiredFiles, ...this.distribution(tool)?.requiredFiles ?? []];
    const present = item && (await Promise.all(required.map(path => exists(join(item.root, path))))).every(Boolean);
    return {
      kind: tool.kind, version: tool.version,
      selected: (await this.current(tool.kind)).version === tool.version,
      status: !this.distribution(tool) ? "unsupported host" : !item ? "not installed" : !present ? "tools missing" : "installed",
      tools: item?.root ?? this.configuredPath(tool) ?? join(this.root, "tools", tool.kind, `${tool.version}-${this.host}`),
      configuredPaths: this.locations[tool.kind]?.[tool.version] ?? {},
      pendingPaths: pending ? { path: configured } : {},
      executable: item && tool.executable ? join(item.root, tool.executable) : null,
      application: item && tool.application ? join(item.root, tool.application.path) : null,
      path: item && this.canRun(tool) ? tool.path.map(path => resolve(item.root, path)) : [],
      host: this.host, canRun: this.canRun(tool), source: this.distribution(tool)?.source ?? null,
      release: tool.release ?? null, upstream: tool.upstream ?? null,
    };
  }
  async environment(inherited: Env, selected?: Tool): Promise<Env> {
    const environment = { ...inherited };
    const paths: string[] = [];
    const active = selected ? [selected] : await Promise.all([...new Set(this.entries.map(item => item.kind))]
      .map(name => this.current(name)));
    for (const tool of active) {
      const item = await this.record(tool);
      if (!item) {
        if (selected) throw new Error(`${tool.kind} ${tool.version} is not installed; run gizos install ${tool.kind} ${tool.version}`);
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
