import { afterEach, expect, test } from "bun:test";
import { chmod, cp, mkdir, mkdtemp, readFile, readdir, readlink, rm, stat, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  GizloaderBootstrap, loaderRequest, parseLoaderChecksums, parseLoaderIndex, type LoaderIndex, type LoaderPlatform,
} from "../src/gizloader";
import { exists, type Command } from "../src/common";
import { Resources, tools } from "../src/resources";

const temporary: string[] = [];
afterEach(async () => { await Promise.all(temporary.splice(0).map(path => rm(path, { recursive: true, force: true }))); });
const digest = (text: string) => createHash("sha256").update(text).digest("hex");
async function directory() {
  const root = await mkdtemp(join(process.env.TEST_TMPDIR ?? tmpdir(), "gizloader-test-"));
  temporary.push(root);
  return root;
}
const bytes = "a local verified installer fixture";
function index(platform: LoaderPlatform = "darwin-arm64", snapshot = false): LoaderIndex {
  return {
    schema: 1, version: "1.2.3", channel: snapshot ? "snapshot" : "release", commit: "a".repeat(40),
    artifacts: [{
      platform, file: `gizloader-1.2.3-${platform === "darwin-arm64" ? "macos-arm64.dmg" : "windows-x64-setup.exe"}`,
      format: platform === "darwin-arm64" ? "dmg" : "nsis", sha256: digest(bytes), size: Buffer.byteLength(bytes),
      ...(platform === "darwin-arm64" ? { app: "Gizloader.app" } : {}),
    }],
  };
}
function sums(value: LoaderIndex) {
  return `${digest(JSON.stringify(value))}  gizloader-index.json\n` + value.artifacts.map(artifact => `${artifact.sha256}  ${artifact.file}\n`).join("");
}
async function local(root: string, value = index()) {
  const metadata = join(root, "input");
  await mkdir(metadata);
  const paths = { index: join(metadata, "index.json"), checksums: join(metadata, "SHA256SUMS"), archive: join(metadata, value.artifacts[0].file) };
  await writeFile(paths.index, JSON.stringify(value));
  await writeFile(paths.checksums, sums(value));
  await writeFile(paths.archive, bytes);
  return paths;
}
const offline = async () => { throw new Error("Network must not be used"); };
async function macFixture(root: string, version = "1.2.3") {
  const app = join(root, "payload/Gizloader.app");
  await mkdir(join(app, "Contents/MacOS"), { recursive: true });
  await writeFile(join(app, "Contents/Info.plist"), JSON.stringify({
    CFBundleIdentifier: "com.gizclaw.gizloader", CFBundleShortVersionString: version.split(/[-+]/)[0],
    GizloaderVersion: version, CFBundleExecutable: "gizloader",
  }));
  for (const name of ["gizloader", "gizloader-cli", "gizloader-session"]) {
    await writeFile(join(app, "Contents/MacOS", name), "#!/bin/sh\nprintf '%s\\n' \"$@\"\nexit 23\n");
    await chmod(join(app, "Contents/MacOS", name), 0o755);
  }
  const calls: string[][] = [];
  const command: Command = async args => {
    calls.push(args);
    if (args[0] === "/usr/bin/hdiutil") {
      if (args[1] === "attach") {
        expect(args).toContain("-readonly");
        expect(args).toContain("-nobrowse");
        await cp(app, join(args[args.indexOf("-mountpoint") + 1], "Gizloader.app"), { recursive: true });
      }
      return "";
    }
    if (args[0] === "/usr/bin/plutil") return JSON.parse(await readFile(args[4], "utf8"))[args[2]];
    throw new Error(`Unexpected command ${args[0]}`);
  };
  return { app, calls, command };
}
function github(value: LoaderIndex, payload = bytes) {
  const requests: string[] = [];
  const tag = value.channel === "snapshot" ? "gizloader-snapshot" : `gizloader-v${value.version}`;
  const base = `https://github.com/GizClaw/gizos/releases/download/${tag}/`;
  const assets = ["gizloader-index.json", "SHA256SUMS", ...value.artifacts.map(item => item.file)].map(name => ({
    name, browser_download_url: base + name, size: value.artifacts.find(item => item.file === name)?.size ?? 1,
  }));
  const release = { tag_name: tag, draft: false, prerelease: value.channel === "snapshot", immutable: false, assets };
  const fetch = async (url: string) => {
    requests.push(url);
    if (url.includes("api.github.com") && url.includes("/releases?")) return Response.json([
      { ...release, tag_name: "v20261102-000000" },
      { ...release, tag_name: "gizloader-v9.0.0", draft: true },
      { ...release, tag_name: "gizloader-v8.0.0", prerelease: true },
      { ...release, tag_name: "gizloader-vinvalid" }, release,
    ]);
    if (url.includes("api.github.com") && url.includes("/releases/tags/")) return Response.json(release);
    if (url === base + "gizloader-index.json") return new Response(JSON.stringify(value));
    if (url === base + "SHA256SUMS") return new Response(sums(value));
    if (url === base + value.artifacts[0].file) return new Response(payload);
    throw new Error(`Unexpected fetch ${url}`);
  };
  return { requests, fetch };
}

test("no arguments opens desktop, explicit help remains toolbox help and CLI args are preserved", () => {
  expect(loaderRequest([])).toMatchObject({ open: true, args: [] });
  expect(loaderRequest(["--snapshot"])).toMatchObject({ open: true, snapshot: true });
  expect(loaderRequest(["install", "gizloader", "1.2.3"])).toMatchObject({ open: false, version: "1.2.3" });
  expect(loaderRequest(["--root", "/space root", "loader", "--snapshot", "--", "--help", "file name", "--snapshot"]))
    .toMatchObject({ root: "/space root", snapshot: true, args: ["--help", "file name", "--snapshot"] });
  expect(loaderRequest(["loader", "--help"])).toMatchObject({ args: ["--help"] });
  for (const args of [["--help"], ["help"], ["--version"], ["setup", "esp-idf", "6.2"], ["install", "adb"]]) {
    expect(loaderRequest(args)).toBeUndefined();
  }
  expect(() => loaderRequest(["install", "gizloader", "--index", "index.json"])).toThrow("Offline installation");
  expect(() => loaderRequest(["install", "gizloader", "--", "flash"])).toThrow("does not accept");
});

test("index rejects malformed metadata, mismatched channels, duplicate platforms and unsafe filenames", () => {
  expect(parseLoaderIndex(JSON.stringify(index()))).toEqual(index());
  for (const change of [
    { schema: 2 }, { version: "01.2.3" }, { version: "1.2.3-01" }, { commit: "a".repeat(39) },
    { channel: "stable" }, { artifacts: [] }, { artifacts: [index().artifacts[0], index().artifacts[0]] },
  ]) expect(() => parseLoaderIndex(JSON.stringify({ ...index(), ...change }))).toThrow();
  for (const change of [{ file: "../gizloader.dmg" }, { format: "tar" }, { size: 0 }, { sha256: "0" }, { app: "../Other.app" }]) {
    expect(() => parseLoaderIndex(JSON.stringify({ ...index(), artifacts: [{ ...index().artifacts[0], ...change }] }))).toThrow();
  }
  expect(() => parseLoaderIndex(JSON.stringify(index()), { snapshot: true })).toThrow("channel/version");
  expect(() => parseLoaderIndex(JSON.stringify(index()), { version: "1.2.4" })).toThrow("channel/version");
  expect(() => parseLoaderChecksums(`${digest(bytes)}  x\n${digest(bytes)}  x\n`)).toThrow("duplicate");
  expect(() => parseLoaderChecksums(`${digest(bytes)}  ../x\n`)).toThrow();
});

test("offline mac install verifies before mounting, keeps bundle intact, activates atomically and forwards CLI stdout/exit", async () => {
  const root = await directory();
  const inputs = await local(root);
  const fixture = await macFixture(root);
  const launches: { args: string[]; detached?: boolean }[] = [];
  const bootstrap = new GizloaderBootstrap({
    root: join(root, "shared"), home: join(root, "user's home"), platform: "darwin-arm64", fetch: offline,
    command: fixture.command, run: async (args, detached) => { launches.push({ args, detached }); return 23; },
  });
  await writeFile(inputs.archive, "corrupt installer");
  await expect(bootstrap.install(inputs)).rejects.toThrow("checksum mismatch");
  expect(fixture.calls).toHaveLength(0);
  expect(await readdir(join(bootstrap.root, "downloads"))).toHaveLength(0);
  expect(await exists(join(bootstrap.root, "installed-darwin-arm64.json"))).toBe(false);
  await writeFile(inputs.archive, bytes);
  const installed = await bootstrap.install(inputs);
  expect(fixture.calls.filter(args => args[0] === "/usr/bin/hdiutil").map(args => args[1])).toEqual(["attach", "detach"]);
  expect(await readlink(join(bootstrap.home, "Applications/Gizloader.app"))).toBe(installed.application);
  expect((await stat(installed.shim)).mode & 0o111).toBe(0o111);
  const child = Bun.spawn([installed.shim, "flash", "file with spaces", "a'b"], { stdout: "pipe", stderr: "pipe" });
  expect(await new Response(child.stdout).text()).toBe("--cli\nflash\nfile with spaces\na'b\n");
  expect(await child.exited).toBe(23);
  const shim = await readFile(installed.shim, "utf8");
  expect(shim).toContain('if [ "$#" -eq 0 ]');
  expect(shim).toContain("exec /usr/bin/open");
  expect(await bootstrap.launch(installed)).toBe(23);
  expect(launches.at(-1)?.args).toEqual(["/usr/bin/open", installed.application]);
  expect(await bootstrap.launch(installed, ["flash", "with spaces.bin"])).toBe(23);
  expect(launches.at(-1)?.args).toEqual([installed.executable, "--cli", "flash", "with spaces.bin"]);
  expect((await bootstrap.ensure()).application).toBe(installed.application);
  await writeFile(inputs.archive, "corrupt reimport");
  await expect(bootstrap.install(inputs)).rejects.toThrow("checksum mismatch");
  expect(await readlink(join(bootstrap.home, "Applications/Gizloader.app"))).toBe(installed.application);
  expect(fixture.calls.filter(args => args[0] === "/usr/bin/hdiutil")).toHaveLength(2);
});

test("macOS snapshots validate the full GizloaderVersion while accepting a numeric short version", async () => {
  const root = await directory();
  const value = index("darwin-arm64", true);
  value.version = "1.2.3-snapshot.4";
  value.artifacts[0].file = `gizloader-${value.version}-macos-arm64.dmg`;
  const inputs = await local(root, value);
  const fixture = await macFixture(root, value.version);
  const bootstrap = new GizloaderBootstrap({ root, home: join(root, "home"), platform: "darwin-arm64", command: fixture.command, fetch: offline });
  const installed = await bootstrap.install({ ...inputs, snapshot: true });
  expect(installed.index.version).toBe(value.version);
  const plist = join(installed.application, "Contents/Info.plist");
  const metadata = JSON.parse(await readFile(plist, "utf8"));
  metadata.GizloaderVersion = "1.2.3-snapshot.3";
  await writeFile(plist, JSON.stringify(metadata));
  await expect(bootstrap.ensure({ snapshot: true })).rejects.toThrow("GizloaderVersion differs");
});

test("stable discovery accepts immutable:false while skipping GizOS, drafts, prereleases and invalid tags", async () => {
  const root = await directory();
  const fixture = await macFixture(root);
  const remote = github(index());
  const bootstrap = new GizloaderBootstrap({ root, home: join(root, "home"), platform: "darwin-arm64", command: fixture.command, fetch: remote.fetch });
  const installed = await bootstrap.install();
  expect(installed.index.version).toBe("1.2.3");
  expect(remote.requests[0]).toEndWith("/releases?per_page=100&page=1");
  expect(remote.requests.filter(url => url.includes("releases/download/"))).toHaveLength(3);
  expect(remote.requests.every(url => !url.includes("/releases/latest"))).toBe(true);
  const before = remote.requests.length;
  await bootstrap.ensure();
  expect(remote.requests).toHaveLength(before);
  expect((await bootstrap.install({ version: "1.2.3" })).index.version).toBe("1.2.3");
  expect(remote.requests).toContain("https://api.github.com/repos/GizClaw/gizos/releases/tags/gizloader-v1.2.3");
});

test("download corruption, inconsistent checksums and missing installer files never activate an app", async () => {
  const root = await directory();
  const fixture = await macFixture(root);
  const remote = github(index(), "corrupt");
  const bootstrap = new GizloaderBootstrap({ root, home: join(root, "home"), platform: "darwin-arm64", command: fixture.command, fetch: remote.fetch });
  await expect(bootstrap.install()).rejects.toThrow("checksum mismatch");
  expect(fixture.calls).toHaveLength(0);
  expect(await exists(join(root, "home/Applications/Gizloader.app"))).toBe(false);
  const inputs = await local(root);
  await writeFile(inputs.checksums, sums(index()).replace(digest(bytes), "b".repeat(64)));
  await expect(bootstrap.install(inputs)).rejects.toThrow("differs from SHA256SUMS");
  await writeFile(inputs.checksums, sums(index()));
  await writeFile(inputs.index, JSON.stringify({ ...index(), commit: "b".repeat(40) }));
  await expect(bootstrap.install(inputs)).rejects.toThrow("index checksum mismatch");
  await writeFile(inputs.index, JSON.stringify(index()));
  await rm(join(fixture.app, "Contents/MacOS/gizloader-session"));
  await expect(bootstrap.install(inputs)).rejects.toThrow();
  expect(fixture.calls.filter(args => args[0] === "/usr/bin/hdiutil").map(args => args[1])).toEqual(["attach", "detach"]);
  expect(await readdir(join(bootstrap.root, "apps"))).toHaveLength(0);
  expect(await exists(join(bootstrap.root, "installed-darwin-arm64.json"))).toBe(false);
});

test("Windows snapshot installs per-user with NSIS /S and inherits native CLI status without Unix tools", async () => {
  const root = await directory();
  const value = index("win32-x64", true);
  const remote = github(value);
  const localAppData = join(root, "User Name/AppData/Local");
  const installRoot = join(localAppData, "Programs/Gizloader");
  const calls: string[][] = [];
  const launches: { args: string[]; detached?: boolean }[] = [];
  const bootstrap = new GizloaderBootstrap({
    root: join(root, "shared"), home: root, platform: "win32-x64", env: { LOCALAPPDATA: localAppData }, fetch: remote.fetch,
    command: async (args, options) => {
      calls.push(args);
      expect(args).toEqual([args[0], "/S"]);
      expect(args[0]).toEndWith(value.artifacts[0].file);
      expect(await readFile(args[0], "utf8")).toBe(bytes);
      expect(options?.inherit).toBe(true);
      await mkdir(installRoot, { recursive: true });
      for (const name of ["gizloader.exe", "gizloader-cli.exe", "gizloader-session.exe", "gizloader.cmd"]) await writeFile(join(installRoot, name), "shipped executable");
      return "";
    },
    run: async (args, detached) => { launches.push({ args, detached }); return 19; },
  });
  const installed = await bootstrap.install({ snapshot: true });
  expect(remote.requests[0]).toEndWith("/releases/tags/gizloader-snapshot");
  expect(calls).toHaveLength(1);
  expect(installed.application).toBe(installRoot);
  expect(installed.shim).toBe(join(installRoot, "gizloader.cmd"));
  expect(await bootstrap.launch(installed, ["--help"])).toBe(19);
  expect(launches.at(-1)).toEqual({ args: [join(installRoot, "gizloader.exe"), "--cli", "--help"], detached: undefined });
  await bootstrap.launch(installed);
  expect(launches.at(-1)).toEqual({ args: [join(installRoot, "gizloader.exe")], detached: true });
  await bootstrap.ensure({ snapshot: true });
  expect(calls).toHaveLength(1);
  expect(await readdir(join(bootstrap.root, "downloads"))).toHaveLength(0);
});

test("unsupported desktop hosts fail before fetch and retired loader catalog entries cannot install", async () => {
  const root = await directory();
  expect(() => new GizloaderBootstrap({ root, platform: "linux-x64", fetch: offline })).toThrow("SDK commands remain available");
  for (const kind of ["h2loader", "h6loader"]) {
    const tool = tools.find(tool => tool.kind === kind)!;
    const resources = new Resources(join(root, kind), async () => { throw new Error("No builds or downloads allowed"); }, [tool], "darwin-arm64");
    await expect(resources.setup(tool)).rejects.toThrow("standalone installation is retired");
    await expect(resources.importArchive(tool, "/unused")).rejects.toThrow("standalone installation is retired");
    expect(await resources.record(tool)).toBeUndefined();
  }
});
