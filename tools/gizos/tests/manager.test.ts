import { afterEach, describe, expect, test } from "bun:test";
import { mkdtemp, mkdir, readFile, writeFile, rm, chmod, readdir, realpath, symlink } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir, hostname } from "node:os";
import {
  Manager, catalog, cleanEnvironment, configFile, configureRoot, dataRoot, defaultRoot,
  execute, exists, locked, selectSdk, shellEnvironment, upgrade,
  type Sdk, type Command,
} from "../src/manager";

const temporary: string[] = [];
afterEach(async () => {
  await Promise.all(temporary.splice(0).map(path => rm(path, { recursive: true, force: true })));
});

async function directory() {
  const path = await realpath(await mkdtemp(join(process.env.TEST_TMPDIR ?? tmpdir(), "gizos-test-")));
  temporary.push(path);
  return path;
}
async function git(root: string, ...args: string[]) {
  return execute(["git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
    "-c", "core.hooksPath=/dev/null", "-c", "protocol.file.allow=always", "-C", root, ...args], {
    env: { ...process.env, GIT_CONFIG_GLOBAL: "/dev/null", GIT_CONFIG_NOSYSTEM: "1" },
  });
}
async function script(path: string, text: string) {
  await mkdir(join(path, ".."), { recursive: true });
  await writeFile(path, "#!/bin/sh\nset -eu\n" + text);
  await chmod(path, 0o755);
}
async function fixture() {
  const base = await directory();
  const upstream = join(base, "upstream");
  await mkdir(join(upstream, "tools"), { recursive: true });
  await writeFile(join(upstream, "tools/idf.py"), "# fixture\n");
  await writeFile(join(upstream, "tools/idf_tools.py"), "# fixture\n");
  await writeFile(join(upstream, "Makefile"), "# fixture\n");
  await git(upstream, "init", "--quiet");
  await git(upstream, "add", ".");
  await git(upstream, "commit", "--quiet", "-m", "fixture");
  const commit = (await git(upstream, "rev-parse", "HEAD")).trim();
  const entries: Sdk[] = [
    { kind: "idf", version: "6.2", default: true, repository: upstream, commit, requiredFile: "tools/idf.py", pythonSeries: "6.2", targets: ["esp32s3"] },
    { kind: "idf", version: "6.0.3", repository: upstream, commit, requiredFile: "tools/idf.py", pythonSeries: "6.0", targets: ["esp32s3"] },
    { kind: "bk", version: "3.1.1", default: true, repository: upstream, commit, requiredFile: "Makefile" },
  ];
  const calls: { args: string[]; options: any }[] = [];
  const command: Command = async (args, options = {}) => {
    calls.push({ args, options });
    if (args[0] === "bash" && args[1].endsWith("/install.sh")) {
      const series = options.cwd!.includes("6.0.3-") ? "6.0" : "6.2";
      await script(join(options.env!.IDF_TOOLS_PATH!, "python_env", `idf${series}_py3.14_env/bin/python`), `
printf 'ESP_IDF_VERSION=${series}\\n'
printf 'PATH=%s/tools:%s/bin:%s\\n' "$IDF_PATH" "$IDF_TOOLS_PATH" "$PATH"
`);
      return "";
    }
    return execute(args, {
      ...options, env: {
        ...options.env ?? process.env, GIT_ALLOW_PROTOCOL: "file",
        GIT_CONFIG_GLOBAL: "/dev/null", GIT_CONFIG_NOSYSTEM: "1",
      },
    });
  };
  const manager = new Manager(join(base, "shared store"), command, entries);
  return { base, upstream, entries, calls, manager };
}

describe("catalog and shared storage", () => {
  test("catalog owns exact SDK pins without reading a GizOS checkout", () => {
    for (const entry of catalog.sdks) expect(entry.commit).toMatch(/^[0-9a-f]{40}$/);
    expect(selectSdk("esp", "6.2").kind).toBe("idf");
    expect(selectSdk("bk7258", "3.1.1").kind).toBe("bk");
    expect(() => selectSdk("idf", "6.3")).toThrow("Unsupported");
    expect(() => selectSdk("idf", "../../outside")).toThrow("Unsupported");
  });
  test("configured storage is independent of working directory and supports an override", async () => {
    const root = await directory();
    const env = { XDG_CONFIG_HOME: join(root, "config"), XDG_DATA_HOME: join(root, "data") };
    expect(await dataRoot(undefined, env)).toBe(defaultRoot(env));
    await configureRoot(join(root, "H002 shared SDKs"), env);
    expect(await dataRoot(undefined, env)).toBe(join(root, "H002 shared SDKs"));
    expect(await dataRoot(join(root, "override"), env)).toBe(join(root, "override"));
    expect(await dataRoot(undefined, { ...env, GIZOS_DEVENV_HOME: join(root, "environment") })).toBe(join(root, "environment"));
    expect(JSON.parse(await readFile(configFile(env), "utf8")).root).toContain("H002");
  });
  test("two SDK versions share tools and keep distinct source directories", async () => {
    const { manager } = await fixture();
    const current = await manager.setup(manager.sdk("idf", "6.2"));
    const older = await manager.setup(manager.sdk("idf", "6.0.3"));
    expect(current.sdk).not.toBe(older.sdk);
    expect(current.tools).toBe(older.tools);
    expect(current.python).not.toBe(older.python);
    expect((await manager.environment(manager.sdk("idf", "6.2"))).ESP_IDF_VERSION).toBe("6.2");
    expect((await manager.environment(manager.sdk("idf", "6.0.3"))).ESP_IDF_VERSION).toBe("6.0");
  });
});

describe("provisioning and environment activation", () => {
  test("preserves modified SDKs and refuses to use them", async () => {
    const { manager } = await fixture();
    const sdk = manager.sdk("idf", "6.2");
    const installation = await manager.setup(sdk);
    await writeFile(join(installation.sdk, "tools/idf.py"), "operator changes\n");
    await expect(manager.setup(sdk)).rejects.toThrow("tracked changes");
    await expect(manager.environment(sdk)).rejects.toThrow("tracked changes");
    expect(await readFile(join(installation.sdk, "tools/idf.py"), "utf8")).toBe("operator changes\n");
  });
  test("reuses downloaded SDKs without refetching", async () => {
    const { manager, calls } = await fixture();
    const sdk = manager.sdk("idf", "6.2");
    await manager.setup(sdk);
    await manager.setup(sdk);
    expect(calls.filter(call => call.args.includes("fetch"))).toHaveLength(1);
  });
  test("initializes recursive submodules and detects missing submodule content", async () => {
    const { manager, upstream, base, entries } = await fixture();
    const child = join(base, "component");
    await mkdir(child);
    await git(child, "init", "--quiet");
    await writeFile(join(child, "component.c"), "int component;\n");
    await git(child, "add", ".");
    await git(child, "commit", "--quiet", "-m", "component");
    await git(upstream, "submodule", "add", child, "components/child");
    await git(upstream, "commit", "--quiet", "-m", "add submodule");
    entries[0].commit = (await git(upstream, "rev-parse", "HEAD")).trim();
    const item = await manager.setup(entries[0], { sourcesOnly: true });
    expect(await exists(join(item.sdk, "components/child/component.c"))).toBe(true);
    await rm(join(item.sdk, "components/child"), { recursive: true });
    await expect(manager.setup(entries[0], { sourcesOnly: true })).rejects.toThrow();
  });
  test("concurrent setups complete without partially installed SDKs", async () => {
    const { manager, calls } = await fixture();
    const sdk = manager.sdk("idf", "6.2");
    const items = await Promise.all([manager.setup(sdk), manager.setup(sdk)]);
    expect(items[0].sdk).toBe(items[1].sdk);
    expect(calls.filter(call => call.args.includes("fetch"))).toHaveLength(1);
    expect(await readdir(join(manager.root, "locks"))).toHaveLength(0);
  });
  test("a failed fetch leaves no installed record or partial source", async () => {
    const { manager } = await fixture();
    const sdk = { ...manager.sdk("idf", "6.2"), commit: "a".repeat(40) };
    await expect(manager.setup(sdk)).rejects.toThrow();
    expect(await exists(manager.sourcePath(sdk))).toBe(false);
    expect(await exists(manager.statePath(sdk))).toBe(false);
  });
  test("adopts existing installations without downloading or running installers", async () => {
    const { manager, base, calls, entries } = await fixture();
    const first = await manager.setup(entries[0]);
    const hostPython = join(base, "host-python");
    await writeFile(hostPython, await readFile(first.python!));
    await chmod(hostPython, 0o755);
    await rm(first.python!);
    await symlink(hostPython, first.python!);
    const another = new Manager(join(base, "another store"), manager.command, entries);
    const before = calls.length;
    const adopted = await another.setup(entries[0], {
      sdk: first.sdk, tools: first.tools, python: first.python,
    }, true);
    expect(adopted.sdk).toBe(first.sdk);
    expect(adopted.tools).toBe(first.tools);
    expect(adopted.python).toBe(first.python);
    expect((await another.environment(entries[0])).IDF_PYTHON_ENV_PATH).toBe(
      join(first.tools, "python_env", "idf6.2_py3.14_env"),
    );
    expect(calls.slice(before).some(call => call.args[0] === "bash" || call.args.includes("fetch"))).toBe(false);
    const older = await another.setup(another.sdk("idf", "6.0.3"));
    expect(older.tools).toBe(first.tools);
  });
  test("BK exports SDK and compiler locations and keeps an explicit loader", async () => {
    const { manager, base, upstream } = await fixture();
    const arm = join(base, "arm");
    await script(join(arm, "bin/arm-none-eabi-gcc"), "printf '10.3.1\\n'\n");
    for (const name of ["arm-none-eabi-gcc-ar", "arm-none-eabi-objcopy"]) {
      await script(join(arm, "bin", name), "exit 0\n");
    }
    const sdk = manager.sdk("bk", "3.1.1");
    await manager.setup(sdk, { sdk: upstream, tools: arm }, true);
    const loader = join(base, "loader", "bk_loader");
    await script(loader, "exit 0\n");
    const environment = await manager.environment(sdk, undefined, {
      PATH: "/usr/bin:/bin", BK_LOADER_BIN: loader, H2LOADER_IDF_PATH: "/old-idf",
    });
    expect(environment.BK7258_PATH).toBe(upstream);
    expect(environment.COMPILER_TOOLCHAIN_PATH).toBe(join(arm, "bin"));
    expect(environment.BK_LOADER_BIN).toBe(loader);
    expect(environment.PATH!.split(":")).toContain(join(base, "loader"));
    expect(environment.H2LOADER_IDF_PATH).toBeUndefined();
  });
  test("cleans active SDK paths before installing another version", () => {
    const environment = cleanEnvironment({
      PATH: "/old/tools/gcc/bin:/usr/bin:/bin",
      IDF_TOOLS_PATH: "/old/tools", IDF_PATH: "/old/idf",
      H2LOADER_IDF_PATH: "/old/idf", VIRTUAL_ENV: "/old/python", KEEP: "yes",
    });
    expect(environment.PATH).toBe("/usr/bin:/bin");
    expect(environment.IDF_PATH).toBeUndefined();
    expect(environment.H2LOADER_IDF_PATH).toBeUndefined();
    expect(environment.KEEP).toBe("yes");
  });
  test("switching IDF versions removes the previous Python and compiler paths without accumulating duplicates", async () => {
    const { manager } = await fixture();
    const current = manager.sdk("idf", "6.2");
    const older = manager.sdk("idf", "6.0.3");
    await manager.setup(current);
    await manager.setup(older);
    const inherited = { PATH: "/operator/bin:/usr/bin:/bin" };
    const first = await manager.environment(current, undefined, inherited);
    const repeated = await manager.environment(current, undefined, first);
    expect(repeated.PATH).toBe(first.PATH);
    const changed = await manager.environment(older, undefined, first);
    expect(changed.PATH).not.toContain(manager.sourcePath(current));
    expect(changed.PATH).not.toContain("idf6.2_py");
    expect(changed.PATH).toContain("idf6.0_py");
    expect(changed.PATH).toContain("/operator/bin");
    expect(changed.VIRTUAL_ENV).not.toBe(first.VIRTUAL_ENV);
  });
  test("default activation selects catalog defaults while preserving inherited environments when none are installed", async () => {
    const { manager } = await fixture();
    const inherited = { PATH: "/usr/bin:/bin", IDF_PATH: "/operator/legacy-idf" };
    expect(await manager.defaultEnvironment(inherited)).toEqual({ ...inherited, GIZOS_DEVENV_HOME: manager.root });
    const current = await manager.setup(manager.sdk("idf", "6.2"));
    await manager.setup(manager.sdk("idf", "6.0.3"));
    const active = await manager.defaultEnvironment(inherited);
    expect(active.IDF_PATH).toBe(current.sdk);
    expect(active.H2LOADER_IDF_PATH).toBe(current.sdk);
    expect(active.PATH!.indexOf(current.sdk)).toBeLessThan(active.PATH!.indexOf("/usr/bin"));
  });
  test("shell output quotes special path characters and clears stale SDK variables", async () => {
    const text = shellEnvironment({ PATH: "/path with 'quotes'/$dollar", IDF_PATH: "/SDK" }, { SECRET: "not printed" });
    expect(text).not.toContain("SECRET");
    expect(text).toContain("unset BK7258_PATH");
    const output = await execute(["sh", "-c", text + '\nprintf "%s" "$PATH"'], { env: { PATH: "/bin:/usr/bin" } });
    expect(output).toBe("/path with 'quotes'/$dollar");
  });
  test("upgrade only invokes the package manager, never SDK setup", async () => {
    const calls: string[][] = [];
    await upgrade("2026.10.30-r120000", async args => { calls.push(args); return ""; });
    expect(calls).toEqual([[
      process.execPath, "add", "--global", "@gizclaw/gizos@2026.10.30-r120000",
      "--registry", "https://static-volc.gizclaw.com/npm/",
    ]]);
    await expect(upgrade("--unsafe")).rejects.toThrow("exact package version");
  });
  test("recovers a lock left by an exited process", async () => {
    const root = await directory();
    const child = Bun.spawn([process.execPath, "-e", "process.exit(0)"]);
    await child.exited;
    const lock = join(root, "locks", "stale");
    await mkdir(lock, { recursive: true });
    await writeFile(join(lock, "owner.json"), JSON.stringify({ pid: child.pid, host: hostname() }));
    expect(await locked(root, "stale", async () => 42)).toBe(42);
  });
});

describe("CLI discovery", () => {
  test("env and list work without installing anything or a GizOS checkout", async () => {
    const root = join(await directory(), "uninstalled shared root");
    const cli = join(import.meta.dir, "../src/cli.ts");
    const environment = { ...process.env, GIZOS_DEVENV_HOME: root };
    const output = await execute([process.execPath, cli, "env", "bk", "--json"], { cwd: "/", env: environment });
    const result = JSON.parse(output);
    expect(result.root).toBe(root);
    expect(result.environments[0].kind).toBe("bk");
    expect(result.environments[0].status).toBe("not installed");
    expect(await exists(root)).toBe(false);
    await expect(execute([process.execPath, cli, "setup", "idf", "6.3"], { env: environment })).rejects.toThrow("Unsupported");
    expect(await exists(root)).toBe(false);
  });
});
