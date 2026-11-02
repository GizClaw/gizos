import { chmod, cp, lstat, mkdir, mkdtemp, readFile, rename, rm, symlink } from "node:fs/promises";
import { homedir, tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { createHash, randomUUID } from "node:crypto";
import { atomicJson, execute, exists, locked, quote, type Command, type Env } from "./common";
import { sha256 } from "./resources";

export type LoaderPlatform = "darwin-arm64" | "win32-x64";
export type LoaderChannel = "release" | "snapshot";
export type LoaderArtifact = {
  platform: LoaderPlatform; file: string; sha256: string; size: number; format: "dmg" | "nsis"; app?: string;
};
export type LoaderIndex = {
  schema: 1; version: string; channel: LoaderChannel; commit: string; artifacts: LoaderArtifact[];
};
export type LoaderOptions = {
  snapshot?: boolean; version?: string; archive?: string; index?: string; checksums?: string;
};
export type LoaderRequest = LoaderOptions & { root?: string; open: boolean; args: string[] };
export type LoaderInstallation = { index: LoaderIndex; application: string; executable: string; shim: string };
type ReleaseAsset = { name: string; browser_download_url: string; size: number };
type Release = { tag_name: string; draft: boolean; prerelease: boolean; assets: ReleaseAsset[] };
type Fetch = (url: string, init?: RequestInit) => Promise<Response>;
type Runner = (args: string[], detached?: boolean) => Promise<number>;
const repository = "https://api.github.com/repos/GizClaw/gizos";
const indexFile = "gizloader-index.json";
const sumsFile = "SHA256SUMS";
const application = "Gizloader.app";
const identifier = "com.gizclaw.gizloader";
const semver = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/;

export function loaderRequest(input: string[]): LoaderRequest | undefined {
  const args = [...input];
  // Global --root is accepted before the desktop command as well as after it.
  let root: string | undefined;
  if (args[0] === "--root") {
    root = args[1];
    if (!root || root.startsWith("--")) throw new Error("Missing value for --root");
    args.splice(0, 2);
  }
  const action = args[0];
  if (action === "install" && (args[1] !== "gizloader" || args.includes("--help"))) return;
  if (action && !["loader", "install", "--snapshot"].includes(action)) return;
  const request: LoaderRequest = { root, open: action !== "install", args: [] };
  if (action === "loader") args.shift();
  if (action === "install") args.splice(0, 2);
  for (let index = 0; index < args.length; index++) {
    const argument = args[index];
    if (argument === "--") { request.args.push(...args.slice(index + 1)); break; }
    if (argument === "--snapshot") { request.snapshot = true; continue; }
    const options: Record<string, "root" | "archive" | "index" | "checksums"> = {
      "--root": "root", "--archive": "archive", "--index": "index", "--checksums": "checksums",
    };
    const option = options[argument];
    if (option) {
      const value = args[++index];
      if (!value || value.startsWith("--")) throw new Error(`Missing value for ${argument}`);
      request[option] = value;
    } else if (action === "install") {
      if (request.version || argument.startsWith("--")) throw new Error("Usage: gizos install gizloader [version] [--snapshot]");
      request.version = argument;
    } else if (action === "loader") request.args.push(argument);
    else throw new Error(`Unknown option ${argument}; run gizos --help`);
  }
  if (action === "install" && request.args.length) throw new Error("install does not accept application arguments; use gizos loader -- <arguments>");
  if (request.version && !semver.test(request.version)) throw new Error("Gizloader version must be an exact semantic version");
  if ((request.index || request.checksums) && !(request.index && request.checksums && request.archive)) {
    throw new Error("Offline installation requires --index, --checksums and --archive together");
  }
  return request;
}

export function parseLoaderIndex(text: string, options: LoaderOptions = {}): LoaderIndex {
  let index: LoaderIndex;
  try { index = JSON.parse(text); } catch { throw new Error("Invalid gizloader-index.json JSON"); }
  if (!index || index.schema !== 1 || typeof index.version !== "string" || !semver.test(index.version) ||
      !["release", "snapshot"].includes(index.channel) || !/^[0-9a-f]{40}$/.test(index.commit) ||
      !Array.isArray(index.artifacts) || !index.artifacts.length) {
    throw new Error("Invalid gizloader-index.json contract");
  }
  const channel = options.snapshot ? "snapshot" : "release";
  if (index.channel !== channel || options.version && index.version !== options.version) {
    throw new Error("Gizloader index does not match the requested channel/version");
  }
  const platforms = new Set<string>();
  for (const artifact of index.artifacts) {
    const mac = artifact?.platform === "darwin-arm64";
    if (!artifact || !["darwin-arm64", "win32-x64"].includes(artifact.platform) || platforms.has(artifact.platform) ||
        artifact.format !== (mac ? "dmg" : "nsis") ||
        artifact.file !== `gizloader-${index.version}-${mac ? "macos-arm64.dmg" : "windows-x64-setup.exe"}` ||
        !/^[0-9a-f]{64}$/.test(artifact.sha256) || !Number.isSafeInteger(artifact.size) || artifact.size <= 0 ||
        artifact.app !== undefined && (!mac || artifact.app !== application)) {
      throw new Error("Invalid Gizloader artifact contract");
    }
    platforms.add(artifact.platform);
  }
  return index;
}

export function parseLoaderChecksums(text: string): Map<string, string> {
  const sums = new Map<string, string>();
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue;
    const match = /^([0-9a-f]{64}) [ *]([^/\\\r\n]+)$/.exec(line);
    if (!match || sums.has(match[2])) throw new Error("Invalid or duplicate SHA256SUMS entry");
    sums.set(match[2], match[1]);
  }
  return sums;
}

async function run(args: string[], detached = false): Promise<number> {
  const child = Bun.spawn(args, {
    stdin: detached ? "ignore" : "inherit", stdout: detached ? "ignore" : "inherit",
    stderr: detached ? "ignore" : "inherit",
  });
  if (detached) { child.unref(); return 0; }
  const interrupt = () => { try { child.kill("SIGINT"); } catch {} };
  const terminate = () => { try { child.kill("SIGTERM"); } catch {} };
  process.on("SIGINT", interrupt);
  process.on("SIGTERM", terminate);
  try { return await child.exited; }
  finally { process.off("SIGINT", interrupt); process.off("SIGTERM", terminate); }
}

export class GizloaderBootstrap {
  readonly root: string;
  readonly home: string;
  readonly platform: LoaderPlatform;
  readonly env: Env;
  readonly fetch: Fetch;
  readonly command: Command;
  readonly run: Runner;
  constructor(options: {
    root: string; home?: string; platform?: string; env?: Env; fetch?: Fetch; command?: Command; run?: Runner;
  }) {
    const platform = options.platform ?? `${process.platform}-${process.arch}`;
    if (platform !== "darwin-arm64" && platform !== "win32-x64") {
      throw new Error(`Gizloader desktop is unsupported on ${platform}; supported hosts: macOS arm64 and Windows x64. SDK commands remain available.`);
    }
    this.root = join(resolve(options.root), "gizloader");
    this.home = options.home ?? homedir();
    this.platform = platform;
    this.env = options.env ?? process.env;
    this.fetch = options.fetch ?? ((url, init) => fetch(url, init));
    this.command = options.command ?? execute;
    this.run = options.run ?? run;
  }
  private async response(url: string): Promise<Response> {
    const response = await this.fetch(url, { headers: { Accept: "application/vnd.github+json", "User-Agent": "gizos-gizloader-bootstrap" } });
    if (!response.ok) throw new Error(`Gizloader download failed (${response.status}): ${url}`);
    return response;
  }
  private async release(options: LoaderOptions): Promise<Release> {
    if (options.snapshot || options.version) {
      const tag = options.snapshot ? "gizloader-snapshot" : `gizloader-v${options.version}`;
      const release: Release = await (await this.response(`${repository}/releases/tags/${encodeURIComponent(tag)}`)).json();
      if (release.tag_name !== tag || release.draft || !options.snapshot && release.prerelease) {
        throw new Error("Gizloader stable releases must be published and not prereleases");
      }
      return release;
    }
    // GitHub's generic /releases/latest also contains unrelated GizOS releases.
    for (let page = 1; ; page++) {
      const releases: Release[] = await (await this.response(`${repository}/releases?per_page=100&page=${page}`)).json();
      if (!Array.isArray(releases)) throw new Error("Invalid GitHub releases response");
      const release = releases.find(item => typeof item.tag_name === "string" && item.tag_name.startsWith("gizloader-v") &&
        semver.test(item.tag_name.slice("gizloader-v".length)) && !item.draft && !item.prerelease);
      if (release) return release;
      if (releases.length < 100) throw new Error("No stable Gizloader release is published; use --snapshot for the snapshot channel");
    }
  }
  private asset(release: Release, name: string): ReleaseAsset {
    const assets = release.assets?.filter(asset => asset.name === name);
    if (assets?.length !== 1) throw new Error(`Gizloader release is missing a unique ${name} asset`);
    const asset = assets[0];
    const url = new URL(asset.browser_download_url);
    if (url.origin !== "https://github.com" || url.search || url.hash ||
        decodeURIComponent(url.pathname) !== `/GizClaw/gizos/releases/download/${release.tag_name}/${name}`) {
      throw new Error(`Unexpected Gizloader release asset URL: ${name}`);
    }
    return asset;
  }
  private async metadata(options: LoaderOptions) {
    const release = options.index ? undefined : await this.release(options);
    const [indexText, sumsText] = options.index
      ? await Promise.all([Bun.file(options.index).text(), Bun.file(options.checksums!).text()])
      : await Promise.all([indexFile, sumsFile].map(async name => (await this.response(this.asset(release!, name).browser_download_url)).text()));
    const sums = parseLoaderChecksums(sumsText);
    const indexSum = sums.get(indexFile);
    if (indexSum && createHash("sha256").update(indexText).digest("hex") !== indexSum) {
      throw new Error("Gizloader index checksum mismatch");
    }
    const index = parseLoaderIndex(indexText, options);
    if (release && index.channel === "release" && release.tag_name !== `gizloader-v${index.version}`) {
      throw new Error("Gizloader index version differs from its release tag");
    }
    for (const artifact of index.artifacts) {
      if (sums.get(artifact.file) !== artifact.sha256) throw new Error(`Gizloader artifact differs from SHA256SUMS: ${artifact.file}`);
    }
    const artifact = index.artifacts.find(item => item.platform === this.platform);
    if (!artifact) throw new Error(`Gizloader release has no ${this.platform} artifact`);
    const asset = release ? this.asset(release, artifact.file) : undefined;
    if (asset && asset.size !== artifact.size) throw new Error("Gizloader release asset size differs from index");
    return { index, artifact, url: asset?.browser_download_url };
  }
  private installation(index: LoaderIndex, artifact: LoaderArtifact): LoaderInstallation {
    if (this.platform === "darwin-arm64") {
      const app = join(this.root, "apps", `${index.version}-${artifact.sha256}`, application);
      return { index, application: app, executable: join(app, "Contents/MacOS/gizloader"), shim: join(this.home, ".local/bin/gizloader") };
    }
    const directory = join(this.env.LOCALAPPDATA ?? join(this.home, "AppData/Local"), "Programs/Gizloader");
    return { index, application: directory, executable: join(directory, "gizloader.exe"), shim: join(directory, "gizloader.cmd") };
  }
  private get state() { return join(this.root, `installed-${this.platform}.json`); }
  private async verify(installation: LoaderInstallation): Promise<void> {
    const mac = this.platform === "darwin-arm64";
    const directory = mac ? join(installation.application, "Contents/MacOS") : installation.application;
    const required = mac ? ["gizloader", "gizloader-cli", "gizloader-session"]
      : ["gizloader.exe", "gizloader-cli.exe", "gizloader-session.exe", "gizloader.cmd"];
    for (const name of required) {
      const path = join(directory, name);
      const file = await lstat(path);
      if (!file.isFile() || mac && !(file.mode & 0o111)) throw new Error(`Gizloader executable is missing or invalid: ${path}`);
    }
    if (mac) {
      const info = join(installation.application, "Contents/Info.plist");
      for (const [key, value] of [
        ["CFBundleIdentifier", identifier], ["GizloaderVersion", installation.index.version], ["CFBundleExecutable", "gizloader"],
      ]) {
        const actual = await this.command(["/usr/bin/plutil", "-extract", key, "raw", info]);
        if (actual.trim() !== value) throw new Error(`Gizloader application ${key} differs from ${value}`);
      }
      const shortVersion = await this.command(["/usr/bin/plutil", "-extract", "CFBundleShortVersionString", "raw", info]);
      if (![installation.index.version, installation.index.version.split(/[-+]/)[0]].includes(shortVersion.trim())) {
        throw new Error(`Gizloader application CFBundleShortVersionString differs from ${installation.index.version}`);
      }
    }
  }
  private async activate(installation: LoaderInstallation) {
    if (this.platform !== "darwin-arm64") return;
    const link = join(this.home, "Applications", application);
    await mkdir(dirname(link), { recursive: true });
    const current = await lstat(link).catch((error: any) => { if (error.code !== "ENOENT") throw error; });
    if (current && !current.isSymbolicLink()) throw new Error(`An existing application was preserved: ${link}. Move it before installing the managed Gizloader app.`);
    const temporary = `${link}.${randomUUID()}.tmp`;
    try { await symlink(installation.application, temporary); await rename(temporary, link); }
    finally { await rm(temporary, { force: true }); }
    await mkdir(dirname(installation.shim), { recursive: true });
    const shim = `${installation.shim}.${randomUUID()}.tmp`;
    try {
      await Bun.write(shim, `#!/bin/sh\nif [ "$#" -eq 0 ]; then\n  exec /usr/bin/open ${quote(link)}\nfi\nexec ${quote(join(link, "Contents/MacOS/gizloader"))} --cli "$@"\n`);
      await chmod(shim, 0o755);
      await rename(shim, installation.shim);
    } finally { await rm(shim, { force: true }); }
  }
  async ensure(options: LoaderOptions = {}): Promise<LoaderInstallation> {
    if (!options.archive && !options.index) {
      let text: string | undefined;
      try { text = await readFile(this.state, "utf8"); }
      catch (error: any) { if (error.code !== "ENOENT") throw error; }
      if (text) {
        const saved = JSON.parse(text);
        if (saved.schema !== 1) throw new Error("Invalid Gizloader installation record");
        if (saved.index?.channel === (options.snapshot ? "snapshot" : "release") && (!options.version || saved.index.version === options.version)) {
          const index = parseLoaderIndex(JSON.stringify(saved.index), options);
          const artifact = index.artifacts.find(item => item.platform === this.platform);
          if (!artifact) throw new Error("Invalid Gizloader installation platform");
          const installation = this.installation(index, artifact);
          await this.verify(installation);
          await this.activate(installation);
          return installation;
        }
      }
    }
    return this.install(options);
  }
  async install(options: LoaderOptions = {}): Promise<LoaderInstallation> {
    if (options.version && !semver.test(options.version)) throw new Error("Gizloader version must be an exact semantic version");
    if ((options.index || options.checksums) && !(options.index && options.checksums && options.archive)) {
      throw new Error("Offline installation requires --index, --checksums and --archive together");
    }
    const { index, artifact, url } = await this.metadata(options);
    return locked(this.root, "install", async () => {
      const installation = this.installation(index, artifact);
      const cachedMac = this.platform === "darwin-arm64" && await exists(installation.application);
      if (cachedMac && !options.archive) {
        await this.verify(installation);
      } else {
        // Every download lives in a new empty directory; execute only a verified installer.
        const downloads = join(this.root, "downloads");
        await mkdir(downloads, { recursive: true });
        const temporary = await mkdtemp(join(downloads, "gizloader-"));
        try {
          const archive = join(temporary, artifact.file);
          if (options.archive) await Bun.write(archive, Bun.file(resolve(options.archive)));
          else await Bun.write(archive, await this.response(url!));
          if (Bun.file(archive).size !== artifact.size || await sha256(archive) !== artifact.sha256) {
            throw new Error("Gizloader archive size or SHA-256 checksum mismatch");
          }
          if (cachedMac) await this.verify(installation);
          else if (this.platform === "darwin-arm64") await this.installMac(installation, archive);
          else {
            // The shipped NSIS installer owns the per-user directory and CLI wrapper.
            await this.command([archive, "/S"], { inherit: true });
            await this.verify(installation);
          }
        } finally { await rm(temporary, { recursive: true, force: true }); }
      }
      await this.activate(installation);
      await atomicJson(this.state, { schema: 1, index });
      return installation;
    });
  }
  private async installMac(installation: LoaderInstallation, archive: string) {
    const destination = dirname(installation.application);
    await mkdir(dirname(destination), { recursive: true });
    const staging = await mkdtemp(join(dirname(destination), ".staging-"));
    // hdiutil requires an internal-volume mount point on macOS.
    const mount = await mkdtemp(join(process.platform === "darwin" ? "/private/tmp" : tmpdir(), "gizloader-mount-"));
    let mounted = false;
    try {
      try {
        await this.command(["/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mount, archive]);
        mounted = true;
        if (!(await lstat(join(mount, application))).isDirectory()) throw new Error("Gizloader.app is missing from the disk image");
        const app = join(staging, application);
        await cp(join(mount, application), app, { recursive: true, verbatimSymlinks: true });
        await this.verify({ ...installation, application: app });
      } finally {
        if (mounted) await this.command(["/usr/bin/hdiutil", "detach", mount]);
      }
      await rename(staging, destination);
    } finally {
      await rm(staging, { recursive: true, force: true });
      await rm(mount, { recursive: true, force: true });
    }
  }
  async launch(installation: LoaderInstallation, args: string[] = []): Promise<number> {
    if (args.length) return this.run([installation.executable, "--cli", ...args]);
    return this.platform === "darwin-arm64" ? this.run(["/usr/bin/open", installation.application])
      : this.run([installation.executable], true);
  }
}
