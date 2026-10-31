import { afterEach, describe, expect, test } from "bun:test";
import { mkdtemp, mkdir, readFile, writeFile, rm, chmod, readdir, realpath, rename, symlink } from "node:fs/promises";
import { dirname, join } from "node:path";
import { tmpdir, hostname } from "node:os";
import {
  Manager, catalog, cleanEnvironment, configFile, configuration, configureRoot, dataRoot, defaultRoot,
  execute, exists, locked, sdkConfiguration, pathConfiguration, sdkName, selectSdk, shellEnvironment, upgrade,
  type Sdk, type Command,
} from "../src/manager";
import { selections, withLease } from "../src/store";

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
    expect(selectSdk("bk_avdk_smp", "3.1.1")).toBe(selectSdk("bk", "3.1.1"));
    expect(sdkName(selectSdk("bk_avdk_smp", "3.1.1"))).toBe("bk7258");
    expect(sdkName(selectSdk("bk_avdk_smp", "4.0.1"))).toBe("bk7259");
    expect(() => selectSdk("bk7258", "4.0.1")).toThrow("Unsupported");
    expect(() => selectSdk("bk7259", "3.1.1")).toThrow("Unsupported");
    expect(selectSdk("bk7259", "4.0.1").armVersion).toBe("14.3.rel1");
    expect(selectSdk("bk_idk", "2.0.1-r650e754").chips).toEqual(["bk7236", "bk7258"]);
    expect(selectSdk("bk7236", "2.0.1-r650e754")).toBe(selectSdk("bk_idk", "2.0.1-r650e754"));
    expect(() => selectSdk("bk7258", "2.0.1-r650e754")).toThrow("Unsupported");
    expect(sdkName(selectSdk("esp-idf", "6.2"))).toBe("esp-idf");
    expect(selectSdk("esp_idf", "6.2")).toBe(selectSdk("esp-idf", "6.2"));
    expect(selectSdk("esp_idf", "v6.0.3")).toBe(selectSdk("esp-idf", "6.0.3"));
    expect(selectSdk("esp_idf", "ddb3852")).toBe(selectSdk("esp-idf", "6.2"));
    for (const [name, alias, version] of [
      ["jieli-ac695n", "AC695N_Soundbox_SDK", "76b95777a083"],
      ["jieli-ac707n", "e_badge_707_sdk_200", "d0167685d032"],
      ["jieli-ac791n", "fw-AC791N_SDK", "eb04f1966cf2"],
    ]) expect(selectSdk(name, version)).toBe(selectSdk(alias, version));
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
  test("repository identities separate equal family/version labels while preserving legacy records and shared tools", async () => {
    const { manager, upstream, base, entries } = await fixture();
    const firstSdk = manager.sdk("bk", "3.1.1");
    const arm = join(base, "arm");
    await script(join(arm, "bin/arm-none-eabi-gcc"), "printf '10.3.1\\n'\n");
    for (const name of ["arm-none-eabi-gcc-ar", "arm-none-eabi-objcopy"]) {
      await script(join(arm, "bin", name), "exit 0\n");
    }
    const first = await manager.setup(firstSdk, { sdk: upstream, tools: arm }, true);
    const legacy = join(manager.root, "environments/bk/3.1.1.json");
    await mkdir(join(legacy, ".."), { recursive: true });
    await rename(manager.statePath(firstSdk), legacy);
    const legacyItem = { ...first };
    delete legacyItem.name;
    delete legacyItem.repository;
    await writeFile(legacy, JSON.stringify(legacyItem));
    const original = await readFile(legacy, "utf8");
    expect((await manager.record(firstSdk))?.sdk).toBe(upstream);
    const second = join(base, "second-bk-sdk");
    await mkdir(second);
    await git(second, "init", "--quiet");
    await writeFile(join(second, "Makefile"), "# second repository\n");
    await git(second, "add", ".");
    await git(second, "commit", "--quiet", "-m", "second SDK");
    const secondSdk: Sdk = {
      ...firstSdk, repository: second, default: false, commit: (await git(second, "rev-parse", "HEAD")).trim(),
    };
    entries.push(secondSdk);
    expect(() => manager.sdk("bk", "3.1.1")).toThrow("Ambiguous");
    expect(manager.sdk("second-bk-sdk", "3.1.1")).toBe(secondSdk);
    expect(await manager.record(secondSdk)).toBeUndefined();
    await manager.setup(firstSdk, { sdk: upstream, tools: arm }, true);
    await manager.setup(secondSdk, { sdk: second, tools: arm }, true);
    expect(manager.statePath(firstSdk)).not.toBe(manager.statePath(secondSdk));
    expect((await manager.record(firstSdk))?.tools).toBe((await manager.record(secondSdk))?.tools!);
    expect((await manager.environment(secondSdk)).BK_SDK_DIR).toBe(second);
    expect(await readFile(legacy, "utf8")).toBe(original);
  });
  test("friendly SDK names reuse legacy repository records and cached sources in place", async () => {
    const { manager, entries, calls } = await fixture();
    const sdk = manager.sdk("idf", "6.2");
    const installed = await manager.setup(sdk, { sourcesOnly: true });
    const record = await readFile(manager.statePath(sdk), "utf8");
    const renamed = new Manager(manager.root, manager.command, entries.map(entry =>
      entry.kind === "idf" ? { ...entry, name: "friendly-idf", aliases: ["upstream"] } : entry));
    const selected = renamed.sdk("friendly-idf", "6.2");
    expect(renamed.sourcePath(selected)).toBe(installed.sdk);
    expect((await renamed.record(selected))?.sdk).toBe(installed.sdk);
    const reused = await renamed.setup(selected, { sourcesOnly: true });
    expect(reused.sdk).toBe(installed.sdk);
    expect(reused.tools).toBe(installed.tools);
    expect(reused.name).toBe("friendly-idf");
    expect(calls.filter(call => call.args.includes("fetch"))).toHaveLength(1);
    expect(await readFile(manager.statePath(sdk), "utf8")).toBe(record);
  });
});

describe("independent installation paths", () => {
  test("each SDK version can choose its source and tool paths without changing the shared compiler default", async () => {
    const { manager: original, base, entries } = await fixture();
    const env = { XDG_CONFIG_HOME: join(base, "config") };
    const paths = { sdk: join(base, "external SDK"), tools: join(base, "external tools") };
    await pathConfiguration("idf", "6.2", paths, env, entries);
    expect(await exists(paths.sdk)).toBe(false);
    expect(await exists(paths.tools)).toBe(false);
    expect(await exists(original.root)).toBe(false);
    const manager = await Manager.configured(original.root, original.command, entries, env);
    const current = await manager.setup(manager.sdk("idf", "6.2"), { sdk: undefined, tools: undefined });
    const older = await manager.setup(manager.sdk("idf", "6.0.3"));
    expect(current.sdk).toBe(paths.sdk);
    expect(current.tools).toBe(paths.tools);
    expect(current.python).toStartWith(join(paths.tools, "python_env"));
    expect(older.sdk).not.toBe(current.sdk);
    expect(older.tools).not.toBe(current.tools);
    const pool = JSON.parse(await readFile(manager.poolFile(manager.sdk("idf", "6.2")), "utf8"));
    expect(pool.tools).toBe(older.tools);
    const environment = await manager.environment(manager.sdk("idf", "6.2"));
    expect(environment.IDF_PATH).toBe(paths.sdk);
    expect(environment.IDF_TOOLS_PATH).toBe(paths.tools);
    expect(environment.PATH).toContain(join(paths.sdk, "tools"));
    expect((await manager.setup(manager.sdk("idf", "6.2"))).sdk).toBe(current.sdk);
    const override = join(base, "command-line SDK");
    expect((await manager.setup(manager.sdk("idf", "6.2"), { sdk: override })).sdk).toBe(override);
    expect((await pathConfiguration("idf", "6.2", undefined, env, entries)).paths).toEqual(paths);
  });

  test("a source override leaves default tool sharing intact and reset preserves installed payloads", async () => {
    const { manager: original, base, entries } = await fixture();
    const env = { XDG_CONFIG_HOME: join(base, "config") };
    const sdk = join(base, "independent SDK");
    await pathConfiguration("idf", undefined, { sdk }, env, entries);
    const manager = await Manager.configured(original.root, original.command, entries, env);
    const current = await manager.setup(manager.sdk("idf", "6.2"));
    const older = await manager.setup(manager.sdk("idf", "6.0.3"));
    expect(current.sdk).toBe(sdk);
    expect(current.tools).toBe(older.tools);
    await pathConfiguration("idf", "6.2", null, env, entries);
    const reset = await Manager.configured(original.root, original.command, entries, env);
    expect((await reset.setup(reset.sdk("idf", "6.2"))).sdk).toBe(current.sdk);
    expect(await exists(join(current.sdk, "tools/idf.py"))).toBe(true);
  });

  test("changed paths apply on setup, retain old files and rediscover Python under the new tools directory", async () => {
    const { manager: original, base, entries } = await fixture();
    const sdk = original.sdk("idf", "6.2");
    const previous = await original.setup(sdk);
    const env = { XDG_CONFIG_HOME: join(base, "config") };
    const paths = { sdk: join(base, "replacement SDK"), tools: join(base, "replacement tools") };
    await pathConfiguration("idf", "6.2", paths, env, entries);
    const changed = await Manager.configured(original.root, original.command, entries, env);
    const before = await changed.describe(sdk);
    expect(before.sdk).toBe(previous.sdk);
    expect(before.configuredPaths).toEqual(paths);
    expect(before.pendingPaths).toEqual(paths);
    expect((await changed.environment(sdk)).IDF_PATH).toBe(previous.sdk);
    const current = await changed.setup(sdk);
    expect(current.sdk).toBe(paths.sdk);
    expect(current.tools).toBe(paths.tools);
    expect(current.python).not.toBe(previous.python);
    expect(current.python).toStartWith(paths.tools);
    expect((await changed.describe(sdk)).pendingPaths).toEqual({});
    expect(await exists(previous.python!)).toBe(true);
    expect(await exists(join(previous.sdk, "tools/idf.py"))).toBe(true);
    const invalid = join(base, "modified existing SDK");
    await mkdir(join(invalid, "tools"), { recursive: true });
    await writeFile(join(invalid, "tools/idf.py"), "unmanaged content");
    await pathConfiguration("idf", "6.2", { sdk: invalid }, env, entries);
    const rejected = await Manager.configured(original.root, original.command, entries, env);
    await expect(rejected.setup(sdk)).rejects.toThrow();
    expect((await rejected.record(sdk))?.sdk).toBe(current.sdk);
    expect(await readFile(join(invalid, "tools/idf.py"), "utf8")).toBe("unmanaged content");
  });

  test("path settings preserve source selections and other SDK versions, and reject invalid configuration atomically", async () => {
    const base = await directory();
    const env = { XDG_CONFIG_HOME: join(base, "config") };
    await Promise.all([
      configureRoot(join(base, "shared"), env),
      sdkConfiguration("esp_idf", "Espressif", env),
      pathConfiguration("bk7258", "3.1.1", { sdk: join(base, "bk") }, env),
      pathConfiguration("esp_idf", "6.2", { sdk: join(base, "idf") }, env),
    ]);
    await pathConfiguration("esp-idf", "6.2", { tools: join(base, "idf tools") }, env);
    await pathConfiguration("esp_idf", "6.0.3", { sdk: join(base, "older idf") }, env);
    const current = await pathConfiguration("esp_idf", "6.2", undefined, env);
    expect(current.paths).toEqual({ sdk: join(base, "idf"), tools: join(base, "idf tools") });
    const before = await readFile(configFile(env), "utf8");
    for (const paths of [{ sdk: "" }, { tools: "/tmp/invalid\npath" }, { path: "/tmp/tool" }]) {
      await expect(pathConfiguration("esp_idf", "6.2", paths, env)).rejects.toThrow("Invalid installation paths");
    }
    await expect(pathConfiguration("esp_idf", "6.3", { sdk: join(base, "unknown") }, env)).rejects.toThrow("Unsupported");
    await expect(pathConfiguration("bk_avdk_smp", undefined, { sdk: join(base, "ambiguous") }, env)).rejects.toThrow("Select an SDK");
    expect(await readFile(configFile(env), "utf8")).toBe(before);
    await pathConfiguration("esp_idf", "6.2", null, env);
    const settings = await configuration(env);
    expect(settings.root).toBe(join(base, "shared"));
    expect(settings.repositories!["esp-idf"]).toBe("Espressif");
    expect(settings.paths!["esp-idf"]["6.0.3"].sdk).toBe(join(base, "older idf"));
    expect(settings.paths!.bk7258["3.1.1"].sdk).toBe(join(base, "bk"));
    expect(await exists(join(base, "shared"))).toBe(false);
    await writeFile(configFile(env), JSON.stringify({ paths: { "esp-idf": { "6.2": { sdk: "relative" } } } }));
    await expect(configuration(env)).rejects.toThrow("Invalid installation path configuration");
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
    await git(upstream, "config", "-f", ".gitmodules", "submodule.components/child.ignore", "all");
    await git(upstream, "add", ".gitmodules");
    await git(upstream, "commit", "--quiet", "-m", "add submodule");
    entries[0].commit = (await git(upstream, "rev-parse", "HEAD")).trim();
    const item = await manager.setup(entries[0]);
    expect(await exists(join(item.sdk, "components/child/component.c"))).toBe(true);
    await writeFile(join(item.sdk, "components/child/component.c"), "operator changes\n");
    await expect(manager.setup(entries[0])).rejects.toThrow("tracked changes");
    await expect(manager.environment(entries[0])).rejects.toThrow("tracked changes");
    const preview = await manager.remove("idf", "6.2", true);
    expect(preview.preserve.some(item => item.reason === "source has local changes")).toBe(true);
    await writeFile(join(item.sdk, "components/child/component.c"), "int component;\n");
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
  test("BK chip and multi-chip SDKs share tools but export distinct locators and clear stale selections", async () => {
    const { manager, base, upstream, entries } = await fixture();
    const template = entries.find(entry => entry.kind === "bk")!;
    const specs = [
      { name: "bk7258", version: "3.1.1", sdkVariable: "BK7258_PATH", chips: ["bk7258"] },
      { name: "bk7259", version: "4.0.1", sdkVariable: "BK7259_PATH", armVersion: "14.3.rel1", chips: ["bk7259"] },
      { name: "bk_idk", version: "2.0.1-r650e754", sdkVariable: "BK_IDK_PATH", chips: ["bk7236", "bk7258"] },
    ];
    manager.entries = specs.map(spec => ({ ...template, ...spec, default: true }));
    const arm = join(base, "arm");
    await script(join(arm, "bin/arm-none-eabi-gcc"), "printf '10.3.1\\n'\n");
    for (const name of ["arm-none-eabi-gcc-ar", "arm-none-eabi-objcopy"]) {
      await script(join(arm, "bin", name), "exit 0\n");
    }
    const newerArm = join(base, "arm-14.3");
    await script(join(newerArm, "bin/arm-none-eabi-gcc"), "printf '14.3.1\\n'\n");
    for (const name of ["arm-none-eabi-gcc-ar", "arm-none-eabi-objcopy"]) {
      await script(join(newerArm, "bin", name), "exit 0\n");
    }
    const sources: string[] = [];
    for (const sdk of manager.entries) {
      const source = join(base, sdkName(sdk));
      await git(base, "clone", "--quiet", upstream, source);
      sources.push(source);
      await manager.setup(sdk, { sdk: source, tools: sdk.armVersion ? newerArm : arm }, true);
    }
    const all = await manager.defaultEnvironment({ PATH: "/usr/bin:/bin" });
    expect(all.BK7258_PATH).toBe(sources[0]);
    expect(all.BK7259_PATH).toBe(sources[1]);
    expect(all.BK_IDK_PATH).toBe(sources[2]);
    expect(all.BK_SDK_DIR).toBe(sources[0]);
    expect(all.COMPILER_TOOLCHAIN_PATH).toBe(join(arm, "bin"));
    expect(all.GIZOS_BK_TOOLCHAIN_ROOT).toBe(arm);
    const current = await manager.environment(manager.entries[1], undefined, all);
    expect(current.BK7259_PATH).toBe(sources[1]);
    expect(current.BK_SDK_DIR).toBe(sources[1]);
    expect(current.COMPILER_TOOLCHAIN_PATH).toBe(join(newerArm, "bin"));
    expect(current.GIZOS_BK_TOOLCHAIN_ROOT).toBe(newerArm);
    expect(current.BK7258_PATH).toBeUndefined();
    expect(current.BK_IDK_PATH).toBeUndefined();
    const idk = await manager.environment(manager.entries[2], undefined, current);
    expect(idk.BK_IDK_PATH).toBe(sources[2]);
    expect(idk.BK7258_PATH).toBeUndefined();
    expect(idk.BK7259_PATH).toBeUndefined();
    expect(idk.COMPILER_TOOLCHAIN_PATH).toBe(join(arm, "bin"));
    expect(manager.toolsPath(manager.entries[0])).toBe(manager.toolsPath(manager.entries[2]));
    expect(manager.poolFile(manager.entries[0])).not.toBe(manager.poolFile(manager.entries[1]));
    expect((await manager.record(manager.entries[1]))?.armVersion).toBe("14.3.rel1");
  });
  test("a legacy BK7259 record using GNU Arm 10.3 cannot be reused as a ready 14.3 environment", async () => {
    const { manager, base, upstream, entries } = await fixture();
    const sdk: Sdk = {
      ...entries.find(entry => entry.kind === "bk")!, name: "bk7259", aliases: ["bk_avdk_smp"],
      version: "4.0.1", sdkVariable: "BK7259_PATH", armVersion: "14.3.rel1",
    };
    manager.entries = [sdk];
    const oldTools = join(base, "arm-10.3");
    await script(join(oldTools, "bin/arm-none-eabi-gcc"), "printf '10.3.1\\n'\n");
    const legacy = join(manager.root, "environments/bk_avdk_smp/4.0.1.json");
    await mkdir(join(legacy, ".."), { recursive: true });
    await writeFile(legacy, JSON.stringify({
      schema: 1, name: "bk_avdk_smp", kind: "bk", version: sdk.version, commit: sdk.commit,
      sdk: upstream, tools: oldTools, ready: true, targets: [],
    }));
    expect((await manager.describe(sdk)).status).toBe("tools missing");
    await expect(manager.environment(sdk)).rejects.toThrow("compiler mismatch");
    const prepared = await manager.setup(sdk, { sourcesOnly: true });
    expect(prepared.sdk).toBe(upstream);
    expect(prepared.ready).toBe(false);
    expect(prepared.armVersion).toBe("14.3.rel1");
    expect(prepared.tools).not.toBe(oldTools);
    expect(await exists(join(oldTools, "bin/arm-none-eabi-gcc"))).toBe(true);
    expect(JSON.parse(await readFile(legacy, "utf8")).tools).toBe(oldTools);
  });
  test("compiler archives downloaded by the host HTTPS client still require the pinned checksum", async () => {
    const base = await directory();
    let downloads = 0;
    const manager = new Manager(base, async args => {
      expect(args[0]).toBe("python3");
      expect(args.at(-2)).toStartWith("https://armkeil.blob.core.windows.net/");
      await writeFile(args.at(-1)!, "corrupt compiler archive");
      downloads++;
      return "";
    });
    const tools = join(base, "arm-14.3");
    await expect(manager.installArm(tools, manager.sdk("bk7259", "4.0.1"))).rejects.toThrow("checksum mismatch");
    expect(downloads).toBe(1);
    expect(await exists(tools)).toBe(false);
    expect(await readdir(join(base, "downloads"))).toEqual([]);
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
  test("setup selects a version, cached setup switches back, and explicit activation leaves the selection intact", async () => {
    const { manager } = await fixture();
    const inherited = { PATH: "/usr/bin:/bin", IDF_PATH: "/operator/legacy-idf" };
    expect(await manager.defaultEnvironment(inherited)).toEqual({ ...inherited, GIZOS_DEVENV_HOME: manager.root });
    const current = await manager.setup(manager.sdk("idf", "6.2"));
    const older = await manager.setup(manager.sdk("idf", "6.0.3"));
    const active = await manager.defaultEnvironment(inherited);
    expect(active.IDF_PATH).toBe(older.sdk);
    expect((await manager.currentSdk("idf")).version).toBe("6.0.3");
    expect((await manager.environment(manager.sdk("idf", "6.2"))).IDF_PATH).toBe(current.sdk);
    expect((await manager.currentSdk("idf")).version).toBe("6.0.3");
    const installations = manager.command;
    manager.command = (args, options) => {
      if (args[0] === "bash" && args[1].endsWith("install.sh")) throw new Error("Cached setup must not reinstall");
      return installations(args, options);
    };
    await manager.setup(manager.sdk("idf", "6.2"));
    expect((await manager.defaultEnvironment(inherited)).IDF_PATH).toBe(current.sdk);
    expect(await exists(older.sdk)).toBe(true);
    expect(active.H2LOADER_IDF_PATH).toBe(older.sdk);
    expect(active.PATH!.indexOf(older.sdk)).toBeLessThan(active.PATH!.indexOf("/usr/bin"));
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
  test("concurrent tasks and processes recover an abandoned lock without overlapping callbacks", async () => {
    const root = await directory();
    const dead = Bun.spawn([process.execPath, "-e", "process.exit(0)"]);
    await dead.exited;
    const lock = join(root, "locks", "stale");
    await mkdir(lock, { recursive: true });
    await writeFile(join(lock, "owner.json"), JSON.stringify({ pid: dead.pid, host: hostname() }));
    let running = 0;
    let peak = 0;
    const tasks = await Promise.allSettled(Array.from({ length: 8 }, () => locked(root, "stale", async () => {
      peak = Math.max(peak, ++running);
      try { await Bun.sleep(30); }
      finally { running--; }
    })));
    expect(tasks.every(result => result.status === "fulfilled")).toBe(true);
    expect(peak).toBe(1);
    await mkdir(lock, { recursive: true });
    await writeFile(join(lock, "owner.json"), JSON.stringify({ pid: dead.pid, host: hostname() }));
    const common = join(import.meta.dir, "../src/common.ts");
    const children = Array.from({ length: 8 }, (_, index) => Bun.spawn([process.execPath, "-e", `
import { locked, exists } from ${JSON.stringify(common)};
import { mkdir, writeFile, rm } from "node:fs/promises";
import { join } from "node:path";
const root = ${JSON.stringify(root)};
await writeFile(join(root, "ready-${index}"), "");
while (!await exists(join(root, "go"))) await Bun.sleep(5);
await locked(root, "stale", async () => {
  const active = join(root, "active");
  await mkdir(active);
  try {
    await Bun.sleep(40);
    await writeFile(join(root, "completed-${index}"), "");
  } finally { await rm(active, { recursive: true }); }
});
`], { stdout: "ignore", stderr: "pipe" }));
    try {
      const deadline = Date.now() + 10_000;
      while ((await readdir(root)).filter(name => name.startsWith("ready-")).length !== children.length) {
        if (Date.now() > deadline) throw new Error("Lock contenders did not start");
        await Bun.sleep(10);
      }
      await writeFile(join(root, "go"), "");
      const results = await Promise.all(children.map(async child => ({
        code: await child.exited, error: await new Response(child.stderr).text(),
      })));
      expect(results).toEqual(children.map(() => ({ code: 0, error: "" })));
      expect((await readdir(root)).filter(name => name.startsWith("completed-"))).toHaveLength(children.length);
      expect(await readdir(join(root, "locks"))).toEqual([]);
    } finally {
      for (const child of children) child.kill();
      await Promise.all(children.map(child => child.exited));
    }
  });
  test("lock release preserves a replacement directory", async () => {
    const root = await directory();
    const lock = join(root, "locks", "replaced");
    await locked(root, "replaced", async () => {
      await rename(lock, `${lock}-original`);
      await mkdir(lock);
      await writeFile(join(lock, "successor"), "preserve");
    });
    expect(await readFile(join(lock, "successor"), "utf8")).toBe("preserve");
  });
});

describe("version removal and managed cache cleanup", () => {
  test("removing the last SDK clears its inherited activation and preserves operator paths", async () => {
    const { manager } = await fixture();
    await manager.setup(manager.sdk("idf", "6.2"));
    const inherited = { PATH: "/operator/bin:/usr/bin:/bin", KEEP: "yes" };
    const active = await manager.defaultEnvironment(inherited);
    expect(active.GIZOS_ENV_PATHS).toBeTruthy();
    await manager.remove("idf", "6.2");
    const environment = await manager.defaultEnvironment(active);
    expect(environment.PATH).toBe(inherited.PATH);
    expect(environment.KEEP).toBe("yes");
    for (const key of ["IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH", "H2LOADER_IDF_PATH", "VIRTUAL_ENV"]) {
      expect(environment[key]).toBeUndefined();
    }
    expect(shellEnvironment(environment, active)).toContain("unset IDF_PATH");
    expect(environment.GIZOS_DEVENV_HOME).toBe(manager.root);
  });
  test("remove previews without changes, removes only one version and falls back to another installed version", async () => {
    const { manager } = await fixture();
    const current = await manager.setup(manager.sdk("idf", "6.2"));
    const older = await manager.setup(manager.sdk("idf", "6.0.3"));
    const state = await readFile(manager.statePath(manager.sdk("idf", "6.0.3")), "utf8");
    const preview = await manager.remove("idf", "6.0.3", true);
    expect(preview.remove.map(item => item.path)).toContain(older.sdk);
    expect(await readFile(manager.statePath(manager.sdk("idf", "6.0.3")), "utf8")).toBe(state);
    expect(await exists(older.python!)).toBe(true);
    const removed = await manager.remove("idf", "6.0.3");
    expect(removed.remove.map(item => item.path)).toContain(older.sdk);
    expect(await exists(older.sdk)).toBe(false);
    expect(await exists(older.python!)).toBe(false);
    expect(await exists(current.python!)).toBe(true);
    expect(await exists(current.tools)).toBe(true);
    expect((await manager.currentSdk("idf")).version).toBe("6.2");
    expect((await manager.defaultEnvironment()).IDF_PATH).toBe(current.sdk);
    expect((await manager.prune(true)).remove).toEqual([]);
    await manager.remove("idf", "6.2");
    expect((await manager.prune(true)).remove.map(item => item.path)).toContain(current.tools);
    expect(await exists(current.tools)).toBe(true);
    await manager.prune();
    expect(await exists(current.tools)).toBe(false);
  });

  test("adopted payloads and unknown legacy directories are retained after unregistering", async () => {
    const { manager, base, entries } = await fixture();
    const original = await manager.setup(entries[0]);
    const adopted = new Manager(join(base, "adopted store"), manager.command, entries);
    await adopted.setup(entries[0], { sdk: original.sdk, tools: original.tools, python: original.python }, true);
    const result = await adopted.remove("idf", "6.2");
    expect(result.remove).toEqual([]);
    expect(result.preserve.some(item => item.reason.includes("ownership is unknown"))).toBe(true);
    expect(await adopted.record(entries[0])).toBeUndefined();
    expect(await exists(original.sdk)).toBe(true);
    expect(await exists(original.python!)).toBe(true);
    const legacy = join(adopted.root, "tools/unknown/large-payload");
    await mkdir(legacy, { recursive: true });
    await adopted.prune();
    expect(await exists(legacy)).toBe(true);
  });

  test("versions in the same IDF series retain their shared Python environment until the last reference is removed", async () => {
    const { manager, entries } = await fixture();
    const original = manager.sdk("idf", "6.0.3");
    entries.push({ ...original, version: "6.0.3-rfixture" });
    const first = await manager.setup(original);
    const compatible = await manager.setup(manager.sdk("idf", "6.0.3-rfixture"));
    expect(compatible.python).toBe(first.python);
    const result = await manager.remove("idf", "6.0.3");
    expect(result.preserve.some(item => item.path === dirname(dirname(first.python!)))).toBe(true);
    expect(await exists(first.python!)).toBe(true);
    await manager.remove("idf", "6.0.3-rfixture");
    expect(await exists(first.python!)).toBe(false);
  });

  test("a shared checkout remains until every referring version has been removed", async () => {
    const { manager } = await fixture();
    const first = await manager.setup(manager.sdk("idf", "6.2"), { sourcesOnly: true });
    await manager.setup(manager.sdk("idf", "6.0.3"), { sdk: first.sdk, sourcesOnly: true });
    const result = await manager.remove("idf", "6.2");
    expect(result.remove).toEqual([]);
    expect(result.preserve[0].reason).toContain("referenced");
    expect(await exists(first.sdk)).toBe(true);
    await manager.remove("idf", "6.0.3");
    expect(await exists(first.sdk)).toBe(false);
  });

  test("modified sources and directories replaced by the user are retained", async () => {
    const { manager } = await fixture();
    const first = await manager.setup(manager.sdk("idf", "6.2"), { sourcesOnly: true });
    await writeFile(join(first.sdk, "tools/idf.py"), "operator changes\n");
    const changed = await manager.remove("idf", "6.2");
    expect(changed.preserve[0].reason).toBe("source has local changes");
    expect(await exists(first.sdk)).toBe(true);
    const second = await manager.setup(manager.sdk("idf", "6.0.3"), { sourcesOnly: true });
    await rename(second.sdk, `${second.sdk}-preserved`);
    await mkdir(second.sdk);
    await writeFile(join(second.sdk, "user-file"), "keep");
    const replaced = await manager.remove("idf", "6.0.3");
    expect(replaced.preserve[0].reason).toBe("path was replaced");
    await manager.prune();
    expect(await readFile(join(first.sdk, "tools/idf.py"), "utf8")).toBe("operator changes\n");
    expect(await readFile(join(second.sdk, "user-file"), "utf8")).toBe("keep");
  });

  test("running command leases retain payloads until the command exits, and empty-store preview is read-only", async () => {
    const { manager, base } = await fixture();
    const empty = new Manager(join(base, "empty"));
    expect((await empty.prune(true)).remove).toEqual([]);
    expect(await exists(empty.root)).toBe(false);
    const installed = await manager.setup(manager.sdk("idf", "6.2"), { sourcesOnly: true });
    await withLease(manager.root, [installed.sdk], async () => {
      await manager.remove("idf", "6.2");
      expect(await exists(installed.sdk)).toBe(true);
      expect((await manager.prune(true)).remove).toEqual([]);
    });
    expect((await manager.prune(true)).remove.map(item => item.path)).toContain(installed.sdk);
    await manager.prune();
    expect(await exists(installed.sdk)).toBe(false);
  });

  test("registered tag and commit aliases resolve to one canonical installation and reject ambiguous commits", async () => {
    const { manager, entries } = await fixture();
    entries[0].tags = ["fixture-tag"];
    const first = await manager.setup(manager.sdk("idf", "fixture-tag"), { sourcesOnly: true });
    expect(first.version).toBe("6.2");
    expect(manager.statePath(manager.sdk("idf", "fixture-tag"))).toBe(manager.statePath(entries[0]));
    expect(() => manager.sdk("idf", entries[0].commit)).toThrow("Ambiguous");
    entries.splice(1, 1);
    expect(manager.sdk("idf", entries[0].commit.slice(0, 8))).toBe(entries[0]);
    await manager.remove("idf", entries[0].commit);
    expect(await exists(first.sdk)).toBe(false);
    expect(await selections(manager.root)).toEqual({});
  });
});

describe("SDK repository configuration", () => {
  test("catalog defaults, root and repository selections survive updates and concurrent writes", async () => {
    const base = await directory();
    const env = { XDG_CONFIG_HOME: join(base, "config"), XDG_DATA_HOME: join(base, "data") };
    const initial = await sdkConfiguration("bk7258", undefined, env);
    expect(initial.repository).toBe("https://github.com/GizClaw/bk_avdk_smp.git");
    expect(initial.repositoryId).toBe("GizClaw/bk_avdk_smp");
    expect(initial.repositoryAlias).toBe("GizClaw");
    expect(initial.configured).toBe(false);
    expect(await exists(configFile(env))).toBe(false);
    await sdkConfiguration("bk7258", "Beken", env);
    expect(await dataRoot(undefined, env)).toBe(defaultRoot(env));
    const root = join(base, "shared SDKs");
    await Promise.all([
      configureRoot(root, env),
      sdkConfiguration("esp_idf", "Espressif", env),
    ]);
    const settings = await configuration(env);
    expect(settings.root).toBe(root);
    expect(settings.repositories).toEqual({
      bk7258: "Beken",
      "esp-idf": "Espressif",
    });
    const manager = await Manager.configured(root, execute, catalog.sdks, env);
    const sdk = manager.sdk("bk7258", "3.1.1");
    expect(sdkName(sdk)).toBe("bk7258");
    expect(sdk.repositoryAlias).toBe(settings.repositories!.bk7258);
    expect(sdk.repository).toBe("https://github.com/bekencorp/bk_avdk_smp.git");
    expect(manager.statePath(sdk)).toBe(new Manager(root).statePath(selectSdk("bk7258", "3.1.1")));
    const reset = await sdkConfiguration("bk7258", null, env);
    expect(reset).toEqual(initial);
    expect((await configuration(env)).root).toBe(root);
    expect((await configuration(env)).repositories!["esp-idf"]).toBe(settings.repositories!["esp-idf"]);
    expect(await exists(root)).toBe(false);
  });

  test("setup fetches a catalogued fork and later selections reuse cached exact commits", async () => {
    const { manager: original, base, upstream, entries, calls } = await fixture();
    const env = { XDG_CONFIG_HOME: join(base, "config") };
    const mirror = join(base, "renamed-sdk-fork.git");
    await git(base, "clone", "--bare", upstream, mirror);
    const missingCommit = join(base, "empty-fork.git");
    await git(base, "init", "--bare", missingCommit);
    for (const entry of entries) entry.repositories = [
      { id: "original/sdk", url: upstream }, { id: "mirror/renamed-sdk", url: mirror },
      { id: "missing/empty-sdk", url: missingCommit },
    ];
    await sdkConfiguration("upstream", "mirror/renamed-sdk", env, entries);
    const manager = await Manager.configured(original.root, original.command, entries, env);
    const sdk = manager.sdk("upstream", "6.2");
    expect(sdk.repository).toBe(mirror);
    expect(sdkName(sdk)).toBe("upstream");
    const installed = await manager.setup(sdk, { sourcesOnly: true });
    expect((await git(installed.sdk, "remote", "get-url", "origin")).trim()).toBe(mirror);
    expect((await git(installed.sdk, "rev-parse", "HEAD")).trim()).toBe(entries[0].commit);
    await sdkConfiguration("upstream", "missing/empty-sdk", env, entries);
    const changed = await Manager.configured(original.root, original.command, entries, env);
    const reused = await changed.setup(changed.sdk("upstream", "6.2"), { sourcesOnly: true });
    expect(reused.sdk).toBe(installed.sdk);
    expect(reused.repository).toBe(mirror);
    expect(calls.filter(call => call.args.includes("fetch"))).toHaveLength(1);
    const info = await changed.describe(changed.sdk("upstream", "6.2"));
    expect(info.repository).toBe(missingCommit);
    expect(info.installedRepository).toBe(mirror);
    await expect(changed.setup(changed.sdk("upstream", "6.0.3"), { sourcesOnly: true })).rejects.toThrow();
    expect(await changed.record(changed.sdk("upstream", "6.0.3"))).toBeUndefined();
    expect((await git(installed.sdk, "rev-parse", "HEAD")).trim()).toBe(entries[0].commit);
  });

  test("unlisted repositories and arbitrary URLs cannot alter configuration or expose credentials", async () => {
    const base = await directory();
    const env = { XDG_CONFIG_HOME: join(base, "config") };
    await configureRoot(join(base, "shared"), env);
    const initial = await readFile(configFile(env), "utf8");
    for (const repository of [
      "other-org/bk_avdk_smp", "espressif/esp-idf", "--upload-pack=unexpected",
      "https://github.com/bekencorp/bk_avdk_smp.git", "https://private-token@example.invalid/sdk.git",
      "ssh://git:private-password@example.invalid/sdk.git", "https://example.invalid/sdk.git?token=private-token",
    ]) {
      let message = "";
      try { await sdkConfiguration("bk7258", repository, env); }
      catch (error: any) { message = error.message; }
      expect(message).toContain("Repository is not in the catalog");
      expect(message).toContain("gizos list bk7258");
      expect(message).not.toContain("private-token");
      expect(message).not.toContain("private-password");
      expect(await readFile(configFile(env), "utf8")).toBe(initial);
    }
  });
  test("stored settings must resolve to the catalog, and known legacy URLs and SDK names remain readable", async () => {
    const base = await directory();
    const env = { XDG_CONFIG_HOME: join(base, "config") };
    const root = join(base, "shared");
    await configureRoot(root, env);
    await writeFile(configFile(env), JSON.stringify({
      root, repositories: { e_badge_707_sdk_200: "git@github.com:h2vivi/e_badge_707_sdk_200.git" },
    }));
    const legacy = await sdkConfiguration("jieli-ac707n", undefined, env);
    expect(legacy.repositoryId).toBe("h2vivi/e_badge_707_sdk_200");
    const manager = await Manager.configured(root, execute, catalog.sdks, env);
    expect(manager.sdk("ac707n", "d0167685d032").repository).toBe(legacy.repository);
    await sdkConfiguration("jieli-ac707n", "Jieli", env);
    expect((await configuration(env)).repositories).toEqual({
      "jieli-ac707n": "Jieli",
    });
    await writeFile(configFile(env), JSON.stringify({
      root, repositories: { bk7258: "https://private-token@example.invalid/sdk.git" },
    }));
    await expect(Manager.configured(root, execute, catalog.sdks, env)).rejects.toThrow("Repository is not in the catalog");
    await sdkConfiguration("bk7258", null, env);
    expect((await configuration(env)).root).toBe(root);
    expect(await exists(root)).toBe(false);
  });
  test("chip selections migrate a shared legacy BK setting without resetting the other chip", async () => {
    const base = await directory();
    const env = { XDG_CONFIG_HOME: join(base, "config") };
    const root = join(base, "shared");
    await configureRoot(root, env);
    await writeFile(configFile(env), JSON.stringify({
      root, repositories: { bk_avdk_smp: "bekencorp/bk_avdk_smp", "esp-idf": "GizClaw/esp-idf" },
    }));
    expect((await sdkConfiguration("bk7258", undefined, env)).repositoryAlias).toBe("Beken");
    expect((await sdkConfiguration("bk7259", undefined, env)).repositoryAlias).toBe("Beken");
    const reset = await sdkConfiguration("bk7258", null, env);
    expect(reset.repositoryAlias).toBe("GizClaw");
    expect(reset.configured).toBe(false);
    expect((await sdkConfiguration("bk7259", undefined, env)).repositoryAlias).toBe("Beken");
    await sdkConfiguration("bk_idk", "GizClaw", env);
    expect((await configuration(env)).repositories).toEqual({
      bk7259: "Beken", bk_idk: "GizClaw", "esp-idf": "GizClaw/esp-idf",
    });
    expect((await configuration(env)).root).toBe(root);
    expect(await exists(root)).toBe(false);
    await expect(sdkConfiguration("bk_avdk_smp", "GizClaw", env)).rejects.toThrow("Select an SDK");
  });
});

describe("CLI discovery", () => {
  test("config saves independent SDK and tool version paths and env reports them without installing", async () => {
    const base = await directory();
    const root = join(base, "shared");
    const cli = join(import.meta.dir, "../src/cli.ts");
    const env = { ...process.env, GIZOS_DEVENV_HOME: root, XDG_CONFIG_HOME: join(base, "config") };
    const run = (...args: string[]) => execute([process.execPath, cli, ...args], { cwd: "/", env });
    const sdk = join(base, "SDKs/esp-idf-6.2");
    const compiler = join(base, "tools/esp-idf");
    const loader = join(base, "tools/bk-loader");
    const saved = JSON.parse(await run("config", "esp_idf", "6.2", "--sdk", sdk, "--tools", compiler));
    expect(saved).toEqual({ name: "esp-idf", version: "6.2", paths: { sdk, tools: compiler } });
    await run("config", "bk7258", "--sdk", join(base, "SDKs/bk7258"));
    await run("config", "bk-loader", "--path", loader);
    const configured = JSON.parse(await run("config"));
    expect(configured.paths.bk7258["3.1.1"].sdk).toBe(join(base, "SDKs/bk7258"));
    expect(Object.values(configured.paths["bk-loader"])[0]).toEqual({ path: loader });
    expect(JSON.parse(await run("config", "esp_idf")).paths["6.2"]).toEqual(saved.paths);
    expect(JSON.parse(await run("config", "esp_idf", "6.2"))).toEqual(saved);
    const info = JSON.parse(await run("env", "esp_idf", "6.2", "--json")).environments[0];
    expect(info.sdk).toBe(sdk);
    expect(info.tools).toBe(compiler);
    expect(info.status).toBe("not installed");
    const older = JSON.parse(await run("env", "esp_idf", "6.0.3", "--json")).environments[0];
    expect(older.sdk).not.toBe(sdk);
    expect(older.tools).not.toBe(compiler);
    expect(await run("env", "bk-loader")).toContain(loader);
    const before = await readFile(configFile(env), "utf8");
    await expect(run("config", "esp_idf", "6.2", "--path", loader)).rejects.toThrow("Invalid installation paths");
    await expect(run("config", "esp_idf", "6.2", "--reset-paths", "--sdk", sdk)).rejects.toThrow("Usage:");
    expect(await readFile(configFile(env), "utf8")).toBe(before);
    await run("config", "esp_idf", "6.2", "--reset-paths");
    expect(JSON.parse(await run("config", "esp_idf", "6.2")).paths).toEqual({});
    expect((await run("config", "bk-loader"))).toContain(loader);
    expect(await exists(root)).toBe(false);
    expect(await exists(sdk)).toBe(false);
    expect(await exists(compiler)).toBe(false);
    expect(await exists(loader)).toBe(false);
  });

  test("describe explains an SDK in English, accepts aliases and does not install or write configuration", async () => {
    const base = await directory();
    const root = join(base, "shared");
    const cli = join(import.meta.dir, "../src/cli.ts");
    const env = { ...process.env, GIZOS_DEVENV_HOME: root, XDG_CONFIG_HOME: join(base, "config") };
    const run = (...args: string[]) => execute([process.execPath, cli, ...args], { cwd: "/", env });
    const text = await run("describe", "esp_idf");
    expect(text).toContain("Espressif IoT Development Framework");
    expect(text).toContain("GizClaw -> GizClaw/esp-idf");
    expect(text).toContain("Espressif -> espressif/esp-idf");
    expect(text).toContain("Pinned versions:");
    expect(text).toContain("gizos config esp-idf GizClaw");
    expect(text).toContain("gizos setup esp-idf 6.2");
    expect(text).toContain("https://docs.espressif.com/projects/esp-idf/");
    expect(text).not.toMatch(/\p{Script=Han}/u);
    const sdk = JSON.parse(await run("describe", "esp_idf", "--json"));
    expect(sdk.name).toBe("esp-idf");
    expect(sdk.chips).toContain("esp32s3");
    expect(sdk.versions.find((entry: any) => entry.default).version).toBe("6.2");
    const idk = JSON.parse(await run("describe", "bk_idk", "--json"));
    expect(idk.chips).toEqual(["bk7236", "bk7258"]);
    expect(idk.description).toContain("multiple chips");
    await expect(run("describe")).rejects.toThrow("Usage: gizos describe");
    expect(await exists(configFile(env))).toBe(false);
    expect(await exists(root)).toBe(false);
  });

  test("list discovers chip and multi-chip SDK sources, and config selects source aliases without installing SDKs", async () => {
    const base = await directory();
    const config = join(base, "config");
    const shared = join(base, "shared");
    const cli = join(import.meta.dir, "../src/cli.ts");
    const env = {
      ...process.env, XDG_CONFIG_HOME: config, XDG_DATA_HOME: join(base, "data"),
      GIZOS_DEVENV_HOME: shared,
    };
    const run = (...args: string[]) => execute([process.execPath, cli, ...args], { env });
    expect(JSON.parse(await run("config", "esp_idf")).repository).toBe("https://github.com/GizClaw/esp-idf.git");
    expect(await exists(configFile(env))).toBe(false);
    const repository = "bekencorp/bk_avdk_smp";
    const choices = JSON.parse(await run("list", "bk7258", "--json")).repositories[0];
    expect(choices.name).toBe("bk7258");
    expect(choices.chips).toEqual(["bk7258"]);
    expect(choices.repositories.map((entry: any) => entry.alias)).toEqual(["GizClaw", "Beken"]);
    expect(choices.repositories[0]).toMatchObject({ default: true, selected: true });
    expect(JSON.parse(await run("config", "bk7258", "Beken")).repositoryId).toBe(repository);
    await run("config", "bk7259", "GizClaw");
    const idk = JSON.parse(await run("config", "bk_idk", "gizclaw"));
    expect(idk.repositoryAlias).toBe("GizClaw");
    expect(idk.repositoryId).toBe("GizClaw/bk_idk");
    expect(idk.chips).toEqual(["bk7236", "bk7258"]);
    await run("config", "bk7236", "Beken");
    expect(JSON.parse(await run("config", "bk_idk")).repositoryAlias).toBe("Beken");
    await run("config", "bk_idk", "GizClaw");
    await run("config", "esp_idf", "Espressif");
    await run("config", "set", "root", shared);
    const all = JSON.parse(await run("config"));
    expect(all.repositories.bk7258).toBe("Beken");
    expect(all.repositories.bk7259).toBe("GizClaw");
    expect(all.repositories.bk_idk).toBe("GizClaw");
    expect(all.repositories["esp-idf"]).toBe("Espressif");
    const info = JSON.parse(await run("env", "bk7258", "3.1.1", "--json")).environments[0];
    expect(info.name).toBe("bk7258");
    expect(info.repositoryId).toBe(repository);
    expect(info.repository).toBe("https://github.com/bekencorp/bk_avdk_smp.git");
    expect(info.defaultRepository).toBe("https://github.com/GizClaw/bk_avdk_smp.git");
    expect(info.status).toBe("not installed");
    const listed = JSON.parse(await run("list", "bk7258", "--json")).repositories[0];
    expect(listed.repositories[1]).toMatchObject({ default: false, selected: true });
    const text = await run("list", "bk7258");
    expect(text).toContain("* Beken -> bekencorp/bk_avdk_smp (selected)");
    expect(text).toContain("GizClaw -> GizClaw/bk_avdk_smp (default)");
    expect(text).toContain("3.1.1 (selected): not installed");
    expect(await run("list", "jieli-ac707n")).toContain("e_badge/e_badge_707_sdk_200");
    const before = await readFile(configFile(env), "utf8");
    await expect(run("config", "bk7258", "other-org/bk_avdk_smp")).rejects.toThrow("Repository is not in the catalog");
    expect(await readFile(configFile(env), "utf8")).toBe(before);
    const reset = JSON.parse(await run("config", "bk7258", "--reset"));
    expect(reset.repository).toBe(reset.defaultRepository);
    expect(reset.configured).toBe(false);
    expect((await configuration(env)).root).toBe(shared);
    expect(await exists(shared)).toBe(false);
  });

  test("env and list work without installing anything or a GizOS checkout", async () => {
    const root = join(await directory(), "uninstalled shared root");
    const cli = join(import.meta.dir, "../src/cli.ts");
    const environment = { ...process.env, GIZOS_DEVENV_HOME: root, XDG_CONFIG_HOME: join(root, "config") };
    const output = await execute([process.execPath, cli, "env", "bk7258", "--json"], { cwd: "/", env: environment });
    const result = JSON.parse(output);
    expect(result.root).toBe(root);
    expect(result.environments[0].kind).toBe("bk");
    expect(result.environments[0].name).toBe("bk7258");
    expect(result.environments[0].repository).toBe("https://github.com/GizClaw/bk_avdk_smp.git");
    const family = JSON.parse(await execute([process.execPath, cli, "env", "bk", "--json"], { env: environment }));
    expect(family.environments.filter((item: any) => item.name === "bk7258")).toEqual(result.environments);
    expect(result.environments[0].status).toBe("not installed");
    expect(await exists(root)).toBe(false);
    const tag = JSON.parse(await execute([process.execPath, cli, "env", "esp_idf", "v6.0.3", "--json"], { env: environment }));
    expect(tag.environments[0].version).toBe("6.0.3");
    const commit = JSON.parse(await execute([process.execPath, cli, "env", "esp_idf", "ddb3852", "--json"], { env: environment }));
    expect(commit.environments[0].version).toBe("6.2");
    const cleanup = JSON.parse(await execute([process.execPath, cli, "prune", "--dry-run", "--json"], { env: environment }));
    expect(cleanup).toEqual({ dryRun: true, remove: [], preserve: [] });
    await expect(execute([process.execPath, cli, "prune", "--path", "/unrelated"], { env: environment }))
      .rejects.toThrow("only accept");
    await expect(execute([process.execPath, cli, "remove", "esp_idf", "6.2", "--", "extra"], { env: environment }))
      .rejects.toThrow("only accept");
    expect(await exists(root)).toBe(false);
    await expect(execute([process.execPath, cli, "setup", "idf", "6.3"], { env: environment })).rejects.toThrow("Unsupported");
    expect(await exists(root)).toBe(false);
  });
});
