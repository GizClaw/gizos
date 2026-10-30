import { afterEach, expect, test } from "bun:test";
import { chmod, mkdir, mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { Resources, type Tool } from "../src/resources";
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
  } finally { server.stop(true); }
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
