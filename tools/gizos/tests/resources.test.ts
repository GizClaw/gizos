import { afterEach, expect, test } from "bun:test";
import { chmod, cp, mkdir, mkdtemp, readFile, readdir, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { Resources, tools, type Tool } from "../src/resources";
import { cleanEnvironment, Manager, shellEnvironment } from "../src/manager";
import { execute, exists, type Command } from "../src/common";

const temporary: string[] = [];
afterEach(async () => {
  await Promise.all(temporary.splice(0).map(path => rm(path, { recursive: true, force: true })));
});
async function directory() {
  const root = await mkdtemp(join(process.env.TEST_TMPDIR ?? tmpdir(), "gizos-resources-"));
  temporary.push(root);
  return root;
}
async function script(path: string, text: string) {
  await mkdir(join(path, ".."), { recursive: true });
  await writeFile(path, "#!/bin/sh\n" + text);
  await chmod(path, 0o755);
}
const command: Command = (args, options = {}) => execute(args, {
  ...options,
  env: { ...process.env, ...options.env, GIT_CONFIG_GLOBAL: "/dev/null", GIT_CONFIG_NOSYSTEM: "1", GIT_ALLOW_PROTOCOL: "file" },
});
function loader(): Tool {
  return {
    kind: "bk-loader", version: "fixture", default: true, hosts: { any: {} },
    requiredFiles: ["bk_loader", "_internal/runtime"], executable: "bk_loader",
    check: ["--version"], expectedOutput: "fixture-loader",
    exports: { BK_LOADER_BIN: "bk_loader" }, path: ["."],
  };
}

test("a pinned sparse vendor snapshot includes the loader runtime without copying unrelated assets", async () => {
  const root = await directory();
  const upstream = join(root, "upstream");
  await script(join(upstream, "vendor/loader/bk_loader"), "printf 'fixture-loader\\n'\n");
  await mkdir(join(upstream, "vendor/loader/_internal"));
  await writeFile(join(upstream, "vendor/loader/_internal/runtime"), "runtime\n");
  await writeFile(join(upstream, "unrelated-private-payload"), "not needed\n");
  const git = (...args: string[]) => command(["git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-C", upstream, ...args]);
  await git("init", "--quiet");
  await git("add", ".");
  await git("commit", "--quiet", "-m", "vendor fixture");
  const commit = (await git("rev-parse", "HEAD")).trim();
  const tool = loader();
  tool.hosts = { any: { source: { repository: upstream, commit, path: "vendor/loader" } } };
  const resources = new Resources(join(root, "shared"), command, [tool]);
  const installed = await resources.setup(tool);
  expect(await readFile(join(installed.root, "_internal/runtime"), "utf8")).toBe("runtime\n");
  expect(await exists(join(installed.root, "../../unrelated-private-payload"))).toBe(false);
  expect((await resources.setup(tool)).root).toBe(installed.root);
  expect(await readdir(join(resources.root, "locks"))).toHaveLength(0);
  const external = join(root, "separate loader");
  const configured = new Resources(resources.root, command, [tool], undefined, {
    [tool.kind]: { [tool.version]: { path: external } },
  });
  expect((await configured.setup(tool)).root).toBe(external);
  expect(await readFile(join(external, "_internal/runtime"), "utf8")).toBe("runtime\n");
  expect(await exists(join(external, "../../unrelated-private-payload"))).toBe(false);
  expect(await exists(join(installed.root, "_internal/runtime"))).toBe(true);
});

test("adoption validates a complete loader and exports its executable and PATH without installation", async () => {
  const root = await directory();
  const external = join(root, "external loader");
  const tool = loader();
  await script(join(external, "bk_loader"), "printf 'fixture-loader\\n'\n");
  const resources = new Resources(join(root, "shared"), command, [tool]);
  await expect(resources.setup(tool, external)).rejects.toThrow("input is missing");
  expect(await resources.record(tool)).toBeUndefined();
  await mkdir(join(external, "_internal"));
  await writeFile(join(external, "_internal/runtime"), "runtime");
  await resources.setup(tool, external);
  const environment = await resources.environment({ PATH: "/usr/bin:/bin" });
  expect(environment.BK_LOADER_BIN).toBe(join(external, "bk_loader"));
  expect(environment.PATH).toBe(`${external}:/usr/bin:/bin`);
  expect((await resources.describe(tool)).executable).toBe(environment.BK_LOADER_BIN!);
  await rm(join(external, "_internal/runtime"));
  await expect(resources.environment({ PATH: "/usr/bin:/bin" })).rejects.toThrow("input is missing");
});

test("the source loader exports its runnable launcher without putting its private Python on PATH", async () => {
  const root = await directory();
  const tool = tools.find(tool => tool.kind === "bk-loader" && tool.version === "1.1.0.2.post1")!;
  const external = join(root, "python environment");
  await script(join(external, "bin/python"), "exit 0\n");
  await script(join(external, "commands/bk_loader"), "printf 'bk_loader.py,v1.1.0.2\\n'\n");
  const resources = new Resources(join(root, "shared"), command, [tool]);
  await resources.setup(tool, external);
  const environment = await resources.environment({ PATH: "/usr/bin:/bin" }, tool);
  expect(environment.PATH).not.toContain(join(external, "bin"));
  const output = await execute(["sh", "-c", '"$BK_LOADER_BIN" --version; command -v bk_loader'], { env: environment });
  expect(output).toBe(`bk_loader.py,v1.1.0.2\n${environment.BK_LOADER_BIN}`);
});

test("unsupported loader hosts fail before downloads while Linux-only container inputs stay locatable", async () => {
  const root = await directory();
  const tool = loader();
  tool.hosts = { "darwin-arm64": {} };
  const unsupported = new Resources(root, command, [tool], "linux-x64");
  await expect(unsupported.setup(tool)).rejects.toThrow("UNSUPPORTED");
  expect(await exists(root + "/resources")).toBe(false);
  const containerTool: Tool = {
    kind: "jieli-toolchain", version: "fixture", default: true, hosts: { any: {} },
    requiredFiles: ["clang"], exports: { JIELI_TOOLCHAIN_ROOT: "." },
    executableHosts: ["linux-x64"], path: ["."],
  };
  const external = join(root, "linux tools");
  await script(join(external, "clang"), "exit 0\n");
  const resources = new Resources(join(root, "shared"), command, [containerTool], "darwin-arm64");
  await resources.setup(containerTool, external);
  const environment = await resources.environment({ PATH: "/usr/bin:/bin" });
  expect(environment.JIELI_TOOLCHAIN_ROOT).toBe(external);
  expect(environment.PATH).toBe("/usr/bin:/bin");
  expect((await resources.describe(containerTool)).canRun).toBe(false);
});

test("download checksum failure leaves no tool record or partial installation", async () => {
  const root = await directory();
  const server = Bun.serve({ port: 0, fetch: () => new Response("corrupt") });
  try {
    const tool = loader();
    tool.hosts = { any: { url: `http://127.0.0.1:${server.port}/archive`, sha256: "a".repeat(64), format: "tar" } };
    const resources = new Resources(join(root, "shared"), command, [tool]);
    await expect(resources.setup(tool)).rejects.toThrow("checksum mismatch");
    expect(await resources.record(tool)).toBeUndefined();
    expect(await readdir(join(resources.root, "downloads"))).toHaveLength(0);
  } finally { server.stop(true); }
});

test("an imported vendor ZIP is verified, flattened and reused without a network download", async () => {
  const root = await directory();
  await script(join(root, "payload/loader/bk_loader"), "printf 'fixture-loader\\n'\n");
  await mkdir(join(root, "payload/loader/_internal"));
  await writeFile(join(root, "payload/loader/_internal/runtime"), "runtime");
  const archive = join(root, "downloaded loader.zip");
  await execute(["zip", "-qr", archive, "loader"], { cwd: join(root, "payload") });
  const digest = (bytes: Uint8Array) => createHash("sha256").update(bytes).digest("hex");
  const tool = loader();
  tool.hosts = { any: {
    url: "https://offline.invalid/vendor.zip", format: "zip", subdirectory: "loader",
    sha256: digest(await readFile(archive)),
    executableSha256: digest(await readFile(join(root, "payload/loader/bk_loader"))),
  } };
  const resources = new Resources(join(root, "shared"), command, [tool]);
  const corrupt = join(root, "wrong.zip");
  await writeFile(corrupt, "wrong release");
  await expect(resources.importArchive(tool, corrupt)).rejects.toThrow("checksum mismatch");
  expect(await resources.record(tool)).toBeUndefined();
  const cached = await resources.importArchive(tool, archive);
  await writeFile(archive, "the original download is no longer needed");
  const installed = await resources.setup(tool);
  expect(await execute([join(installed.root, "bk_loader"), "--version"])).toBe("fixture-loader");
  expect(await readFile(join(installed.root, "_internal/runtime"), "utf8")).toBe("runtime");
  expect((await resources.setup(tool)).root).toBe(installed.root);
  expect((await resources.environment({ PATH: "/usr/bin:/bin" })).BK_LOADER_BIN).toBe(join(installed.root, "bk_loader"));
  expect(await exists(cached)).toBe(true);
  expect(await exists(`${installed.root}.extract`)).toBe(false);
  expect(await readdir(join(resources.root, "locks"))).toHaveLength(0);
});

(process.platform === "darwin" ? test : test.skip)("a macOS package installs its payload without executing global installation scripts", async () => {
  const root = await directory();
  await script(join(root, "payload/bk_loader"), "printf 'fixture-loader\\n'\n");
  await mkdir(join(root, "payload/_internal"));
  await writeFile(join(root, "payload/_internal/runtime"), "runtime");
  const sideEffect = join(root, "postinstall-ran");
  await script(join(root, "scripts/postinstall"), `touch '${sideEffect}'\n`);
  const archive = join(root, "vendor.pkg");
  const packageRoot = join(root, "package");
  await mkdir(packageRoot);
  // pkgbuild delegates to a helper that cannot write into Bazel's sandbox.
  // A small XAR fixture still exercises the real pkgutil payload expansion.
  await execute([
    "tar", "--format=odc", "-czf", join(packageRoot, "Payload"), "-C", join(root, "payload"), ".",
  ]);
  await execute(["tar", "--format=odc", "-czf", join(packageRoot, "Scripts"), "-C", join(root, "scripts"), "."]);
  await writeFile(join(packageRoot, "PackageInfo"), `<?xml version="1.0" encoding="utf-8"?>
<pkg-info format-version="2" identifier="invalid.example.gizos-loader-test" version="1.0" install-location="/usr/local/lib/bk_loader" auth="root">
  <payload numberOfFiles="3" installKBytes="1"/>
  <scripts><postinstall file="./postinstall"/></scripts>
</pkg-info>
`);
  await execute(["xar", "--compression=none", "-cf", archive, "PackageInfo", "Payload", "Scripts"], { cwd: packageRoot });
  const tool = loader();
  tool.requiredFiles = ["bk_loader"];
  tool.hosts = { any: {
    url: "https://offline.invalid/vendor.pkg", format: "pkg",
    sha256: createHash("sha256").update(await readFile(archive)).digest("hex"),
    requiredFiles: ["_internal/runtime"],
  } };
  const resources = new Resources(join(root, "shared"), command, [tool]);
  await resources.importArchive(tool, archive);
  const installed = await resources.setup(tool);
  expect(await execute([join(installed.root, "bk_loader"), "--version"])).toBe("fixture-loader");
  expect(await exists(sideEffect)).toBe(false);
  expect(await exists(join(installed.root, "Scripts"))).toBe(false);
  await rm(join(installed.root, "_internal/runtime"));
  expect((await resources.describe(tool)).status).toBe("tools missing");
  await expect(resources.setup(tool)).rejects.toThrow("input is missing");
});

test("verified archives install atomically and repeated setup reuses the shared copy", async () => {
  const root = await directory();
  await script(join(root, "payload/bk_loader"), "printf 'fixture-loader\\n'\n");
  await mkdir(join(root, "payload/_internal"));
  await writeFile(join(root, "payload/_internal/runtime"), "runtime");
  const archive = join(root, "fixture.tar");
  await execute(["tar", "-cf", archive, "-C", join(root, "payload"), "."]);
  const bytes = await readFile(archive);
  let downloads = 0;
  const server = Bun.serve({ port: 0, fetch: () => { downloads++; return new Response(bytes); } });
  try {
    const tool = loader();
    tool.hosts = { any: {
      url: `http://127.0.0.1:${server.port}/archive`, sha256: createHash("sha256").update(bytes).digest("hex"), format: "tar",
    } };
    const resources = new Resources(join(root, "shared"), command, [tool]);
    const first = await resources.setup(tool);
    expect((await resources.setup(tool)).root).toBe(first.root);
    expect(downloads).toBe(1);
    expect(await readFile(join(first.root, "_internal/runtime"), "utf8")).toBe("runtime");
    const external = join(root, "custom loader");
    const configured = new Resources(resources.root, command, [tool], undefined, {
      [tool.kind]: { [tool.version]: { path: external } },
    });
    expect((await configured.describe(tool)).tools).toBe(first.root);
    const current = await configured.setup(tool);
    expect(current.root).toBe(external);
    expect((await configured.setup(tool)).root).toBe(current.root);
    const environment = await configured.environment({ PATH: "/usr/bin:/bin" }, tool);
    expect(environment.BK_LOADER_BIN).toBe(join(external, "bk_loader"));
    expect(environment.PATH).toBe(`${external}:/usr/bin:/bin`);
    expect(await readFile(join(first.root, "_internal/runtime"), "utf8")).toBe("runtime");
    expect(downloads).toBe(1);
    class Toolbox extends Manager {
      override get resources() { return configured; }
    }
    const manager = new Toolbox(resources.root, command, []);
    const preview = await manager.remove(tool.kind, tool.version, true);
    expect(preview.remove.map(item => item.path)).toContain(external);
    expect(await exists(join(external, "bk_loader"))).toBe(true);
    await manager.remove(tool.kind, tool.version);
    expect(await exists(external)).toBe(false);
    expect(await exists(first.root)).toBe(true);
    await manager.prune();
    expect(await exists(first.root)).toBe(false);
  } finally { server.stop(true); }
});

test("BK setup installs a checked native loader once while adoption and sources-only setup do not download", async () => {
  const root = await directory();
  const upstream = join(root, "sdk");
  await mkdir(upstream);
  await writeFile(join(upstream, "Makefile"), "# fixture\n");
  const git = (...args: string[]) => command([
    "git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-C", upstream, ...args,
  ]);
  await git("init", "--quiet");
  await git("add", ".");
  await git("commit", "--quiet", "-m", "sdk fixture");
  const sdk = {
    kind: "bk" as const, version: "fixture", default: true, repository: upstream,
    commit: (await git("rev-parse", "HEAD")).trim(), requiredFile: "Makefile",
  };
  const arm = join(root, "arm");
  await script(join(arm, "bin/arm-none-eabi-gcc"), "printf '10.3.1\\n'\n");
  for (const name of ["arm-none-eabi-gcc-ar", "arm-none-eabi-objcopy"]) {
    await script(join(arm, "bin", name), "exit 0\n");
  }
  const entry = join(root, "payload/bin/bk_loader");
  await script(entry, "printf 'fixture-loader\\n'\n");
  const archive = join(root, "loader.tar.gz");
  await execute(["tar", "-czf", archive, "-C", join(root, "payload"), "."]);
  const bytes = await readFile(archive);
  const digest = (data: Uint8Array) => createHash("sha256").update(data).digest("hex");
  let downloads = 0;
  const server = Bun.serve({ port: 0, fetch: () => { downloads++; return new Response(bytes); } });
  try {
    const tool: Tool = {
      ...loader(), requiredFiles: ["bin/bk_loader"], executable: "bin/bk_loader",
      exports: { BK_LOADER_BIN: "bin/bk_loader" }, path: ["bin"],
      hosts: { any: {
        url: `http://127.0.0.1:${server.port}/loader.tar.gz`, sha256: digest(bytes),
        executableSha256: digest(await readFile(entry)), format: "tar",
      } },
    };
    const upgraded = { ...tool, version: "fixture-next", default: false };
    const resources = new Resources(join(root, "shared"), command, [tool, upgraded]);
    class BkManager extends Manager {
      override get resources() { return resources; }
    }
    const manager = new BkManager(resources.root, command, [sdk]);
    await manager.setup(sdk, { sdk: upstream, tools: arm }, true);
    await manager.setup(sdk, { sourcesOnly: true });
    expect(downloads).toBe(0);
    await manager.setup(sdk);
    await manager.setup(sdk);
    expect(downloads).toBe(1);
    await expect(manager.remove(tool.kind, tool.version, true)).rejects.toThrow("required by");
    const previous = (await resources.record(tool))!;
    const next = await resources.setup(upgraded);
    await manager.setup(sdk);
    expect((await manager.record(sdk))?.dependencies).toEqual([next.root]);
    expect(downloads).toBe(1);
    expect(next.root).not.toBe(previous.root);
    expect(await exists(previous.root)).toBe(true);
    const environment = await manager.environment(sdk, undefined, { PATH: "/usr/bin:/bin" });
    expect(await execute(["sh", "-c", "command -v bk_loader; bk_loader --version"], { env: environment }))
      .toBe(`${environment.BK_LOADER_BIN}\nfixture-loader`);
    await writeFile(environment.BK_LOADER_BIN!, "#!/bin/sh\nprintf 'tampered\\n'\n");
    await expect(manager.environment(sdk)).rejects.toThrow("executable checksum mismatch");
  } finally { server.stop(true); }
});

test("native tool installation builds pinned source once and exposes a shared executable", async () => {
  const root = await directory();
  const source = join(root, "source");
  await mkdir(source);
  await writeFile(join(source, "MODULE.bazel"), "# fixture\n");
  const git = (...args: string[]) => command([
    "git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-C", source, ...args,
  ]);
  await git("init", "--quiet");
  await git("add", ".");
  await git("commit", "--quiet", "-m", "native CLI source");
  const commit = (await git("rev-parse", "HEAD")).trim();
  const output = join(root, "bazel-output");
  const tool: Tool = {
    kind: "h2loader", version: "fixture", default: true,
    requiredFiles: ["bin/h2loader"], executable: "bin/h2loader", check: ["--help"], expectedOutput: "h2loader",
    exports: { H2LOADER_BIN: "bin/h2loader" }, path: ["bin"],
    hosts: { any: {
      source: { repository: source, commit, path: "." }, installer: "bazel",
      build: { target: "//cli:h2loader", output: "cli/h2loader" },
    } },
  };
  let builds = 0;
  const tracked: Command = async (args, options) => {
    if (args[1] === "build") {
      builds++;
      expect(args).toContain("//cli:h2loader");
      expect((await command(["git", "-C", options!.cwd!, "rev-parse", "HEAD"])).trim()).toBe(commit);
      await script(join(output, "cli/h2loader"), "printf 'h2loader fixture help\\n'\n");
      return "";
    }
    if (args[1] === "info") return output;
    return command(args, options);
  };
  const resources = new Resources(join(root, "shared"), tracked, [tool]);
  const installed = await resources.setup(tool);
  expect((await resources.setup(tool)).root).toBe(installed.root);
  expect(builds).toBe(1);
  const environment = await resources.environment({ PATH: "/usr/bin:/bin" });
  expect(environment.H2LOADER_BIN).toBe(join(installed.root, "bin/h2loader"));
  expect(await execute(["sh", "-c", "command -v h2loader; h2loader --help"], { env: environment }))
    .toBe(`${environment.H2LOADER_BIN}\nh2loader fixture help`);
});

test("GUI tools retain their application bundle, detach the image and expose a launcher without opening the GUI", async () => {
  const root = await directory();
  const application = "H6 Batch Loader.app";
  const identifier = "com.h6infra.batch-loader";
  const payload = join(root, "payload", application);
  await mkdir(join(payload, "Contents"), { recursive: true });
  await writeFile(join(payload, "Contents/Info.plist"), JSON.stringify({
    CFBundleIdentifier: identifier, CFBundleShortVersionString: "0.2.1",
  }));
  await script(join(payload, "Contents/MacOS/h6-batch-loader"), "exit 1\n");
  const archive = join(root, "gui.dmg");
  await writeFile(archive, "fixture disk image");
  const tool: Tool = {
    kind: "h6loader", version: "0.2.1", default: true,
    application: { path: application, identifier },
    requiredFiles: ["bin/h6loader", `${application}/Contents/Info.plist`, `${application}/Contents/MacOS/h6-batch-loader`],
    executable: "bin/h6loader", check: ["--version"], expectedOutput: "h6loader 0.2.1",
    exports: { H6LOADER_APP_PATH: application }, path: ["bin"],
    hosts: { "darwin-arm64": {
      url: "https://offline.invalid/gui.dmg", format: "dmg", subdirectory: application,
      sha256: createHash("sha256").update(await readFile(archive)).digest("hex"),
    } },
  };
  const images: string[][] = [];
  const tracked: Command = async (args, options) => {
    if (args[0] === "/usr/bin/hdiutil") {
      images.push(args);
      if (args[1] === "attach") {
        expect(args).toContain("-readonly");
        expect(args).toContain("-nobrowse");
        const mount = args[args.indexOf("-mountpoint") + 1];
        await cp(payload, join(mount, application), { recursive: true });
      }
      return "";
    }
    if (args[0] === "/usr/bin/plutil") {
      return JSON.parse(await readFile(args[4], "utf8"))[args[2]];
    }
    return command(args, options);
  };
  const resources = new Resources(join(root, "shared"), tracked, [tool], "darwin-arm64");
  await resources.importArchive(tool, archive);
  const installed = await resources.setup(tool);
  expect(images.map(args => args[1])).toEqual(["attach", "detach"]);
  expect(await execute([join(installed.root, "bin/h6loader"), "--version"])).toBe("h6loader 0.2.1");
  expect(await execute([join(installed.root, "bin/h6loader"), "--help"])).toContain("Usage: h6loader");
  expect((await resources.describe(tool)).application).toBe(join(installed.root, application));
  const environment = await resources.environment({ PATH: "/usr/bin:/bin" }, tool);
  expect(environment.H6LOADER_APP_PATH).toBe(join(installed.root, application));
  expect(environment.PATH).toBe(`${installed.root}/bin:/usr/bin:/bin`);
  expect(await readFile(join(installed.root, "bin/h6loader"), "utf8")).toContain("exec /usr/bin/open");
  await writeFile(join(installed.root, application, "Contents/Info.plist"), JSON.stringify({
    CFBundleIdentifier: identifier, CFBundleShortVersionString: "0.3.0",
  }));
  await expect(resources.setup(tool)).rejects.toThrow("CFBundleShortVersionString differs");
  expect(images).toHaveLength(2);
});

test("a GUI payload extraction failure still detaches its disk image and leaves no installation record", async () => {
  const root = await directory();
  const archive = join(root, "gui.dmg");
  await writeFile(archive, "fixture disk image");
  const tool: Tool = {
    kind: "h6loader", version: "fixture", default: true,
    requiredFiles: ["bin/h6loader"], exports: {}, path: ["bin"],
    hosts: { any: {
      url: "https://offline.invalid/gui.dmg", format: "dmg", subdirectory: "missing.app",
      sha256: createHash("sha256").update(await readFile(archive)).digest("hex"),
    } },
  };
  const images: string[] = [];
  const tracked: Command = async args => {
    if (args[0] !== "/usr/bin/hdiutil") throw new Error("Unexpected command");
    images.push(args[1]);
    return "";
  };
  const resources = new Resources(join(root, "shared"), tracked, [tool]);
  await resources.importArchive(tool, archive);
  await expect(resources.setup(tool)).rejects.toThrow();
  expect(images).toEqual(["attach", "detach"]);
  expect(await resources.record(tool)).toBeUndefined();
  expect(await readdir(join(resources.root, "tools/h6loader"))).toHaveLength(0);
});

test("JieLi config installs new tool directories, resolves checkout aliases and keeps adoption offline", async () => {
  const root = await directory();
  const upstream = join(root, "upstream");
  await script(join(upstream, "toolchain/bin/clang"), "exit 0\n");
  await script(join(upstream, "postbuild/pack"), "exit 0\n");
  await mkdir(join(upstream, "SDK"));
  await writeFile(join(upstream, "SDK/Makefile"), "# fixture\n");
  const git = (...args: string[]) => command([
    "git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-C", upstream, ...args,
  ]);
  await git("init", "--quiet");
  await git("add", ".");
  await git("commit", "--quiet", "-m", "JieLi fixture");
  const commit = (await git("rev-parse", "HEAD")).trim();
  const sdk = {
    name: "jieli-fixture", kind: "ac707n" as const, version: "fixture", default: true,
    repository: upstream, commit, requiredFile: "SDK/Makefile", sdkSubdirectory: "SDK",
  };
  const entries: Tool[] = [
    {
      kind: "jieli-toolchain", version: "fixture", default: true,
      hosts: { any: { source: { repository: upstream, commit, path: "toolchain" } } },
      requiredFiles: ["bin/clang"], exports: { JIELI_TOOLCHAIN_ROOT: "." }, path: [],
    },
    {
      kind: "jieli-postbuild", version: "fixture", default: true,
      hosts: { any: { source: { repository: upstream, commit, path: "postbuild" } } },
      requiredFiles: ["pack"], exports: { JIELI_POSTBUILD_ROOT: "." }, path: [],
    },
  ];
  let fetches = 0;
  const tracked: Command = (args, options) => {
    if (args.includes("fetch")) fetches++;
    return command(args, options);
  };
  class JieliManager extends Manager {
    override get resources() { return new Resources(this.root, tracked, entries, undefined, this.locations); }
  }
  const checkout = join(root, "independent JieLi SDK");
  const compiler = join(root, "independent compiler");
  const postbuild = join(root, "independent postbuild");
  const manager = new JieliManager(join(root, "shared"), tracked, [sdk], {
    [sdk.name]: { fixture: { sdk: checkout, tools: compiler } },
    "jieli-postbuild": { fixture: { path: postbuild } },
  });
  const installed = await manager.setup(sdk);
  const environment = await manager.environment(sdk);
  expect(installed.tools).toBe(compiler);
  expect(environment.JIELI_AC707N_SDK_PATH).toBe(join(checkout, "SDK"));
  expect(environment.JIELI_TOOLCHAIN_ROOT).toBe(compiler);
  expect(environment.JIELI_POSTBUILD_ROOT).toBe(postbuild);
  expect((await manager.describe(sdk)).pendingPaths).toEqual({});
  const alias = join(root, "SDK symlink");
  await symlink(checkout, alias);
  manager.locations[sdk.name].fixture.sdk = alias;
  expect((await manager.describe(sdk)).pendingPaths).toEqual({});
  const pending = join(root, "pending postbuild");
  manager.locations["jieli-postbuild"].fixture.path = pending;
  const before = fetches;
  await manager.setup(sdk, { sdk: checkout, tools: compiler }, true);
  expect(fetches).toBe(before);
  expect(await exists(pending)).toBe(false);
  expect((await manager.resources.describe(entries[1])).pendingPaths).toEqual({ path: pending });
});

test("default activation includes standalone tools, removes only managed PATH entries and is idempotent", async () => {
  const root = await directory();
  const tool = loader();
  const external = join(root, "loader");
  await script(join(external, "bk_loader"), "printf 'fixture-loader\\n'\n");
  await mkdir(join(external, "_internal"));
  await writeFile(join(external, "_internal/runtime"), "runtime");
  class Toolbox extends Manager {
    override get resources() { return new Resources(this.root, command, [tool]); }
  }
  const manager = new Toolbox(join(root, "shared"), command, []);
  await manager.resources.setup(tool, external);
  const original = { PATH: "/operator/bin:/usr/bin:/bin", H2LOADER_PYTHON: "/stale/python" };
  const first = await manager.defaultEnvironment(original);
  const second = await manager.defaultEnvironment(first);
  expect(second).toEqual(first);
  expect(first.BK_LOADER_BIN).toBe(join(external, "bk_loader"));
  expect(first.PATH).toBe(`${external}:/operator/bin:/usr/bin:/bin`);
  expect(cleanEnvironment(first).PATH).toBe(original.PATH);
  expect(first.H2LOADER_PYTHON).toBeUndefined();
  const text = shellEnvironment(first, original);
  for (const shell of ["sh", "bash", ...(process.platform === "darwin" ? ["zsh"] : [])]) {
    const output = await execute([shell, "-c", text + '\nprintf "%s\\n%s" "$BK_LOADER_BIN" "$(command -v bk_loader)"'], { env: original });
    expect(output).toBe(`${first.BK_LOADER_BIN}\n${first.BK_LOADER_BIN}`);
  }
});
