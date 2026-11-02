#!/usr/bin/env bun
import {
  dataRoot, configureRoot, configFile, configuration, sdkConfiguration, sdkChoices, pathConfiguration,
  describeSdk, Manager, PACKAGE, catalog, sdkName, shellEnvironment, upgrade,
} from "./manager";
import { tools } from "./resources";
import { GizloaderBootstrap, loaderRequest } from "./gizloader";
import type { InstallationPaths } from "./common";
import type { Env } from "./common";
import { withLease, withStore } from "./store";
import manifest from "../package.json";
import { delimiter, dirname } from "node:path";

declare const GIZOS_VERSION: string;
const version = typeof GIZOS_VERSION === "string" ? GIZOS_VERSION : manifest.version;
const help = `GizOS desktop installer and SDK toolbox ${version}

Usage:
  gizos                                      Install/open Gizloader desktop
  gizos install gizloader [version] [--snapshot]
  gizos loader [--snapshot] [-- <CLI arguments...>]
  gizos install gizloader --index <json> --checksums <SHA256SUMS> --archive <installer>
  gizos setup <sdk> <version> [--targets esp32s3,esp32p4] [--sources-only]
  gizos install <tool> [version] [--archive <downloaded-archive>]
  gizos remove <sdk|tool> <version> [--dry-run] [--json]
  gizos prune [--dry-run] [--json]
  gizos adopt <sdk> <version> --sdk <checkout> --tools <path> [--python <executable>] [--postbuild <path>]
  gizos adopt <tool> <version> --path <installation-directory>
  gizos list [sdk|tool] [--json]
  gizos describe <sdk> [--json]
  gizos env [sdk|tool] [version] [--json | --shell sh|bash|zsh]
  gizos shell [sdk|tool] [version]
  gizos run [sdk|tool] [version] -- <command> [arguments...]
  gizos config
  gizos config <sdk> [<source-alias> | --reset]
  gizos config <sdk> [version] [--sdk <checkout>] [--tools <directory>] [--python <executable>]
  gizos config <tool> [version] [--path <installation-directory>]
  gizos config <sdk|tool> [version] --reset-paths
  gizos config set root <path>
  gizos upgrade [package-version]
  gizos --version

Global options: --root <shared-directory>
SDKs: ${[...new Set(catalog.sdks.map(sdkName))].join(", ")}
list shows supported chips, source aliases, repositories and versions.
describe explains an SDK and shows usage examples in English.
Legacy SDK names and family aliases remain accepted.
Tools: gizloader, adb, bk-loader, jieli-toolchain, jieli-postbuild
Gizloader supports macOS arm64 and Windows x64; --snapshot selects gizloader-snapshot.
Gizloader installs the latest stable gizloader-v<semver> release by default.
loader arguments run the bundled CLI with inherited stdout and exit status.
Use gizos --help for toolbox help or gizos loader -- --help for native CLI help.
GIZOS_DEVENV_HOME overrides the configured shared directory.
config saves independent SDK/tool version paths; root supplies default locations.
Omitting a version selects the current version, falling back to the catalog default.
setup installs explicit catalog versions; env never installs anything.
setup selects the prepared version without deleting other cached versions.
Catalogued SDK tags and unambiguous commit prefixes also select a pinned version.
remove preserves adopted paths and shared dependencies; prune removes unused managed caches.
BK SDK setup also installs the catalog's platform executable of bk_loader.
install prepares standalone tools; omitting its version selects the catalog default.
upgrade installs/updates the global gizos command from ${PACKAGE} and preserves SDKs.
`;

function parse(args: string[]) {
  const separator = args.indexOf("--");
  const tail = separator < 0 ? [] : args.slice(separator + 1);
  const input = separator < 0 ? args : args.slice(0, separator);
  const flags: Record<string, string | boolean> = {};
  const positional: string[] = [];
  const values = new Set(["root", "sdk", "tools", "python", "targets", "shell", "path", "postbuild", "archive"]);
  const boolean = new Set(["json", "sources-only", "help", "version", "reset", "reset-paths", "dry-run"]);
  for (let i = 0; i < input.length; i++) {
    const argument = input[i];
    if (!argument.startsWith("--")) { positional.push(argument); continue; }
    const key = argument.slice(2);
    if (values.has(key)) {
      const value = input[++i];
      if (!value || value.startsWith("--")) throw new Error(`Missing value for --${key}`);
      flags[key] = value;
    } else if (boolean.has(key)) flags[key] = true;
    else throw new Error(`Unknown option ${argument}`);
  }
  return { flags, positional, tail };
}

async function main() {
  const input = process.argv.slice(2);
  const desktop = loaderRequest(input);
  if (desktop) {
    const root = await dataRoot(desktop.root);
    const bootstrap = new GizloaderBootstrap({ root });
    const installed = desktop.open ? await bootstrap.ensure(desktop) : await bootstrap.install(desktop);
    const bin = dirname(installed.shim);
    if (!desktop.args.length && process.platform === "darwin" && !(process.env.PATH ?? "").split(delimiter).includes(bin)) {
      console.error(`Gizloader launcher: ${installed.shim}\nAdd ${bin} to PATH to use gizloader directly, or use gizos loader.`);
    }
    if (desktop.open) process.exitCode = await bootstrap.launch(installed, desktop.args);
    else console.log(`Gizloader ${installed.index.version}: ready\nApplication: ${installed.application}\nLauncher: ${installed.shim}`);
    return;
  }
  const { flags, positional, tail } = parse(input);
  const [action, requestedName, selected, ...extra] = positional;
  const normalized = requestedName?.replaceAll("_", "-");
  const name = tools.some(tool => tool.kind === normalized) ? normalized : requestedName;
  if (flags.help || action === "help") { console.log(help); return; }
  if (flags.version || action === "version") { console.log(version); return; }
  if (flags.reset && action !== "config") throw new Error("--reset is only supported by gizos config <sdk>");
  if (flags["reset-paths"] && action !== "config") throw new Error("--reset-paths is only supported by gizos config");
  if (flags["dry-run"] && !["remove", "prune"].includes(action)) throw new Error("--dry-run is only supported by remove and prune");
  if (flags.archive && !["install", "setup"].includes(action)) {
    throw new Error("--archive is only supported by gizos install <tool> [version]");
  }
  if (action === "upgrade") { await upgrade(name); console.log(`Updated global ${PACKAGE}; SDK installations were preserved.`); return; }
  if (action === "config" && name === "set" && selected === "root" && extra.length === 1) {
    if (flags.reset || flags["reset-paths"] || ["sdk", "tools", "python", "path", "postbuild"].some(key => flags[key])) {
      throw new Error("Usage: gizos config set root <path>");
    }
    console.log(await configureRoot(extra[0])); return;
  }
  const root = await dataRoot(flags.root as string | undefined);
  if (action === "config") {
    const paths = Object.fromEntries(["sdk", "tools", "python", "path"]
      .filter(key => flags[key] !== undefined).map(key => [key, flags[key]])) as InstallationPaths;
    const hasPaths = Object.keys(paths).length > 0;
    if (flags.postbuild) throw new Error("Configure post-build tools with gizos config jieli-postbuild --path <directory>");
    if ((hasPaths || flags["reset-paths"]) && (!name || flags.reset || flags["reset-paths"] && hasPaths)) {
      throw new Error("Usage: gizos config <sdk|tool> [version] <path-options | --reset-paths>");
    }
    const tool = name && tools.some(tool => tool.kind === name);
    const version = name && selected && (tool
      ? tools.some(tool => tool.kind === name && [tool.version, tool.release?.tag, tool.release?.commit].includes(selected))
      : sdkChoices(name).some(sdk => sdk.version === selected || sdk.tags?.includes(selected) ||
        /^[0-9a-f]{7,40}$/i.test(selected) && sdk.commit.startsWith(selected.toLowerCase())));
    if (hasPaths || flags["reset-paths"] || tool || version) {
      if (extra.length || flags.reset) throw new Error("Use gizos config <sdk|tool> [version] --reset-paths to reset installation paths");
      console.log(JSON.stringify(await pathConfiguration(
        name!, selected, flags["reset-paths"] ? null : hasPaths ? paths : undefined,
        { ...process.env, GIZOS_DEVENV_HOME: root },
      ), null, 2));
      return;
    }
    if (extra.length || flags.reset && (!name || selected)) {
      throw new Error("Usage: gizos config <sdk> [<source-alias> | --reset]");
    }
    if (name) {
      const sdk = await sdkConfiguration(name, flags.reset ? null : selected);
      console.log(JSON.stringify({ ...sdk, paths: (await configuration()).paths?.[sdk.name] ?? {} }, null, 2));
    } else {
      const entries = await Promise.all([...new Set(catalog.sdks.map(sdkName))].map(name => sdkConfiguration(name)));
      console.log(JSON.stringify({
        root, config: configFile(), repositories: Object.fromEntries(entries.map(entry => [entry.name, entry.repositoryAlias])),
        paths: (await configuration()).paths ?? {},
      }, null, 2));
    }
    return;
  }
  if (action === "describe") {
    if (!name || selected || extra.length) throw new Error("Usage: gizos describe <sdk> [--json]");
    const item = await describeSdk(name);
    if (flags.json) console.log(JSON.stringify({ root, ...item }, null, 2));
    else {
      console.log(`${item.name}\n\n${item.description}`);
      if (item.chips.length) console.log(`\nCatalog chips: ${item.chips.join(", ")}`);
      if (item.compiler) console.log(`Compiler: GNU Arm ${item.compiler.version} (${item.compiler.release})`);
      console.log(`\nSources (selected: ${item.repositoryAlias}):`);
      for (const repository of item.repositories) {
        const labels = [repository.default && "default", repository.selected && "selected"].filter(Boolean);
        console.log(`  ${repository.alias} -> ${repository.id}${labels.length ? ` (${labels.join(", ")})` : ""}`);
        console.log(`    ${repository.url}`);
      }
      console.log("\nPinned versions:");
      for (const entry of item.versions) {
        console.log(`  ${entry.version}${entry.default ? " (default)" : ""}: ${entry.commit}`);
        if (entry.chips.length) console.log(`    Chips: ${entry.chips.join(", ")}`);
      }
      console.log("\nUsage:");
      for (const command of item.commands) console.log(`  ${command}`);
      console.log("\nNotes:");
      for (const note of item.notes) console.log(`  - ${note}`);
      if (item.documentation) console.log(`\nDocumentation: ${item.documentation}`);
      console.log(`Shared directory: ${root}`);
    }
    return;
  }
  const manager = await Manager.configured(root);
  const resources = manager.resources;
  if (action === "install" && (!name || !resources.isTool(name))) {
    throw new Error("install requires a tool; run gizos list to see available SDKs and tools");
  }
  if (extra.length) throw new Error("Unexpected arguments; run gizos --help");
  if (action === "remove" || action === "prune") {
    if (tail.length || Object.keys(flags).some(key => !["root", "dry-run", "json"].includes(key))) {
      throw new Error("remove and prune only accept --root, --dry-run and --json options");
    }
    if (action === "remove" && (!name || !selected) || action === "prune" && (name || selected)) {
      throw new Error("Usage: gizos remove <sdk|tool> <version> [--dry-run], or gizos prune [--dry-run]");
    }
    const plan = action === "remove" ? await manager.remove(name!, selected!, Boolean(flags["dry-run"]))
      : await manager.prune(Boolean(flags["dry-run"]));
    if (flags.json) console.log(JSON.stringify(plan, null, 2));
    else {
      if (action === "remove") console.log(`${flags["dry-run"] ? "Would unregister" : "Unregistered"} ${name} ${selected}`);
      for (const item of plan.remove) console.log(`${flags["dry-run"] ? "Would remove" : "Removed"}: ${item.path} (${item.reason})`);
      for (const item of plan.preserve) console.log(`Preserved: ${item.path} (${item.reason})`);
      if (!plan.remove.length) console.log("No managed files to remove.");
    }
    return;
  }
  if (action === "list" || action === "env" && !flags.shell) {
    if (selected && !name) throw new Error("Select an SDK family first");
    const tool = name && resources.isTool(name);
    const sdks = tool ? [] : selected ? [manager.sdk(name, selected)] : manager.sdks(name);
    const tools = !name ? resources.entries.filter(tool => !tool.retired) : tool
      ? selected ? [resources.select(name, selected)] : resources.entries.filter(tool => tool.kind === name) : [];
    const environments = await Promise.all(sdks.map(sdk => manager.describe(sdk)));
    const installations = await Promise.all(tools.map(tool => resources.describe(tool)));
    const repositories = action === "list"
      ? await Promise.all([...new Set(sdks.map(sdkName))].map(name => sdkConfiguration(name))) : undefined;
    if (flags.json) console.log(JSON.stringify({ root, environments, resources: installations, repositories }, null, 2));
    else {
      console.log(`Shared directory: ${root}`);
      for (const sdk of repositories ?? []) {
        console.log(`\n${sdk.name}`);
        if (sdk.chips.length) console.log(`  Chips: ${sdk.chips.join(", ")}`);
        for (const repository of sdk.repositories) {
          const labels = [repository.default && "default", repository.selected && "selected"].filter(Boolean);
          console.log(`  ${repository.selected ? "*" : "-"} ${repository.alias} -> ${repository.id}${labels.length ? ` (${labels.join(", ")})` : ""}`);
          console.log(`    ${repository.url}`);
        }
        console.log("  Versions:");
        for (const item of environments.filter(item => item.name === sdk.name)) {
          console.log(`    ${item.version}${item.selected ? " (selected)" : ""}: ${item.status}`);
        }
      }
      for (const item of action === "env" ? environments : []) {
        console.log(`\n${item.name} ${item.version}${item.selected ? " (selected)" : ""}: ${item.status}`);
        if (action === "env") {
          console.log(`  Repository: ${item.repository}`);
          if (item.installedRepository && item.installedRepository !== item.repository) {
            console.log(`  Installed repository: ${item.installedRepository}`);
          }
          console.log(`  SDK: ${item.sdk}\n  Tools: ${item.tools}`);
          for (const key of ["sdk", "tools", "python"] as const) {
            const pending = item.pendingPaths[key];
            if (pending) console.log(`  Configured ${key}: ${pending} (applied by setup)`);
          }
          if (item.kind === "idf") console.log(`  Python: ${item.python ?? "not installed"}`);
          if (item.kind === "bk") console.log(`  BK Loader: ${item.loader ?? "not installed"}`);
          if (item.path.length) console.log(`  PATH: ${item.path.join(":")}`);
        }
      }
      for (const item of installations) {
        console.log(`\n${item.kind} ${item.version}${item.selected ? " (selected)" : ""}: ${item.status}`);
        if (action === "env") {
          console.log(`  Tools: ${item.tools}`);
          if (item.pendingPaths.path) {
            console.log(`  Configured path: ${item.pendingPaths.path} (applied by setup)`);
          }
          if (item.executable) console.log(`  Executable: ${item.executable}`);
          if (item.application) console.log(`  Application: ${item.application}`);
          if (item.upstream) console.log(`  Upstream: ${item.upstream.version} (CLI ${item.upstream.cliVersion})`);
          if (item.path.length) console.log(`  PATH: ${item.path.join(":")}`);
          if (!item.canRun) console.log("  Host execution: unsupported");
        }
      }
    }
    return;
  }
  if (action === "env" && !name && flags.shell) {
    if (!["bash", "zsh", "sh"].includes(String(flags.shell))) throw new Error("--shell must be bash, zsh, or sh");
    if (flags.json) throw new Error("--json and --shell cannot be combined");
    console.log(shellEnvironment(await manager.defaultEnvironment()));
    return;
  }
  if (!name && !["run", "shell"].includes(action)) throw new Error(`${action} requires an SDK or tool; run gizos --help`);
  const tool = name && resources.isTool(name)
    ? selected ? resources.select(name, selected)
      : action === "install" ? resources.select(name) : await resources.current(name) : undefined;
  const sdk = !name || tool ? undefined : selected ? manager.sdk(name, selected) : await manager.currentSdk(name);
  if (["setup", "install", "adopt"].includes(action)) {
    if (!selected && action !== "install") throw new Error(`${action} requires an explicit catalog version`);
    if (tool) {
      if (action === "adopt" && !flags.path) throw new Error("adopt a tool with --path <installation-directory>");
      if (flags.archive && flags.path) throw new Error("--archive cannot be combined with --path");
      const item = await withStore(root, async () => {
        if (flags.archive) await resources.importArchive(tool, String(flags.archive));
        return resources.setup(tool, flags.path as string | undefined);
      });
      console.log(`${item.kind} ${item.version}: ready\nTools: ${item.root}`);
      return;
    }
    if (flags.archive) throw new Error("--archive is only supported by gizos install <tool> [version]");
    if (action === "adopt" && (!flags.sdk || !flags.tools && !sdk!.kind.startsWith("ac"))) {
      throw new Error("adopt requires --sdk and --tools");
    }
    const targets = flags.targets ? String(flags.targets).split(",") : undefined;
    if (targets && (!targets.length || targets.some(target => !/^[a-z0-9]+$/.test(target)))) {
      throw new Error("Invalid IDF target list");
    }
    const item = await manager.setup(sdk!, {
      sdk: flags.sdk as string | undefined, tools: flags.tools as string | undefined,
      python: flags.python as string | undefined, sourcesOnly: Boolean(flags["sources-only"]), targets,
      postbuild: flags.postbuild as string | undefined,
    }, action === "adopt");
    console.log(`${sdkName(sdk!)} ${item.version}: ${item.ready ? "ready" : "sources ready"}\nSDK: ${item.sdk}\nTools: ${item.tools}`);
    if (item.kind === "bk" && item.ready) console.log(`BK Loader: ${(await manager.describe(sdk!)).loader ?? "not installed"}`);
    return;
  }
  const getEnvironment = () => tool ? manager.toolEnvironment(tool)
    : sdk ? manager.environment(sdk) : manager.defaultEnvironment();
  if (action === "env") {
    if (!["bash", "zsh", "sh"].includes(String(flags.shell))) throw new Error("--shell must be bash, zsh, or sh");
    if (flags.json) throw new Error("--json and --shell cannot be combined");
    console.log(shellEnvironment(await getEnvironment())); return;
  }
  if (action === "run" || action === "shell") {
    const command = action === "run" ? tail : [process.env.SHELL ?? "/bin/sh", "-i"];
    if (!command.length) throw new Error("run requires -- followed by a command");
    let environment: Env;
    await withLease(root, async () => {
      environment = await getEnvironment();
      return [...new Set(Object.entries(environment).flatMap(([key, value]) =>
        value && /(?:PATH|ROOT|_DIR|_BIN|VIRTUAL_ENV)$/.test(key)
          ? value.split(":").filter(path => path.startsWith("/")) : []))];
    }, async () => {
      const child = Bun.spawn(command, { env: environment!, stdin: "inherit", stdout: "inherit", stderr: "inherit" });
      const forward = (signal: NodeJS.Signals) => { try { child.kill(signal); } catch {} };
      const interrupt = () => forward("SIGINT");
      const terminate = () => forward("SIGTERM");
      process.on("SIGINT", interrupt);
      process.on("SIGTERM", terminate);
      try { process.exitCode = await child.exited; }
      finally { process.off("SIGINT", interrupt); process.off("SIGTERM", terminate); }
    });
    return;
  }
  throw new Error(`Unknown command ${action}; run gizos --help`);
}

await main().catch(error => {
  console.error(`gizos: ${error.message}`);
  process.exitCode = 1;
});
