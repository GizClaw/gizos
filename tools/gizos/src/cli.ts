#!/usr/bin/env bun
import { dataRoot, configureRoot, configFile, Manager, PACKAGE, catalog, kind, shellEnvironment, upgrade } from "./manager";
import manifest from "../package.json";

declare const GIZOS_VERSION: string;
const version = typeof GIZOS_VERSION === "string" ? GIZOS_VERSION : manifest.version;
const help = `GizOS SDK and toolchain manager ${version}

Usage:
  gizos setup <sdk|tool> <version> [--targets esp32s3,esp32p4] [--sources-only]
  gizos adopt <sdk> <version> --sdk <checkout> --tools <path> [--python <executable>] [--postbuild <path>]
  gizos adopt <tool> <version> --path <installation-directory>
  gizos list [sdk|tool] [--json]
  gizos env [sdk|tool] [version] [--json | --shell sh|bash|zsh]
  gizos shell [sdk|tool] [version]
  gizos run [sdk|tool] [version] -- <command> [arguments...]
  gizos config [set root <path>]
  gizos upgrade [package-version]
  gizos --version

Global options: --root <shared-directory>
SDKs: idf, bk, ac695n, ac707n, ac791n
Tools: adb, bk-loader, jieli-toolchain, jieli-postbuild
GIZOS_DEVENV_HOME overrides the configured shared directory.
setup installs explicit catalog versions; env never installs anything.
upgrade installs/updates the global gizos command from ${PACKAGE} and preserves SDKs.
`;

function parse(args: string[]) {
  const separator = args.indexOf("--");
  const tail = separator < 0 ? [] : args.slice(separator + 1);
  const input = separator < 0 ? args : args.slice(0, separator);
  const flags: Record<string, string | boolean> = {};
  const positional: string[] = [];
  const values = new Set(["root", "sdk", "tools", "python", "targets", "shell", "path", "postbuild"]);
  const boolean = new Set(["json", "sources-only", "help", "version"]);
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
  const { flags, positional, tail } = parse(process.argv.slice(2));
  const [action, name, selected, ...extra] = positional;
  if (flags.help || action === "help" || !action && !flags.version) { console.log(help); return; }
  if (flags.version || action === "version") { console.log(version); return; }
  if (action === "upgrade") { await upgrade(name); console.log(`Updated global ${PACKAGE}; SDK installations were preserved.`); return; }
  if (action === "config" && name === "set" && selected === "root" && extra.length === 1) {
    console.log(await configureRoot(extra[0])); return;
  }
  const root = await dataRoot(flags.root as string | undefined);
  const manager = new Manager(root);
  const resources = manager.resources;
  if (action === "config") {
    if (name) throw new Error("Usage: gizos config set root <path>");
    console.log(JSON.stringify({ root, config: configFile() }, null, 2)); return;
  }
  if (extra.length) throw new Error("Unexpected arguments; run gizos --help");
  if (action === "list" || action === "env" && !flags.shell) {
    if (selected && !name) throw new Error("Select an SDK family first");
    const tool = name && resources.isTool(name);
    const sdks = tool ? [] : selected ? [manager.sdk(name, selected)]
      : manager.entries.filter(sdk => !name || sdk.kind === kind(name));
    const tools = !name ? resources.entries : tool
      ? selected ? [resources.select(name, selected)] : resources.entries.filter(tool => tool.kind === name) : [];
    const environments = await Promise.all(sdks.map(sdk => manager.describe(sdk)));
    const installations = await Promise.all(tools.map(tool => resources.describe(tool)));
    if (flags.json) console.log(JSON.stringify({ root, environments, resources: installations }, null, 2));
    else {
      console.log(`Shared directory: ${root}`);
      for (const item of environments) {
        console.log(`\n${item.kind} ${item.version}: ${item.status}`);
        if (action === "env") {
          console.log(`  SDK: ${item.sdk}\n  Tools: ${item.tools}`);
          if (item.kind === "idf") console.log(`  Python: ${item.python ?? "not installed"}`);
          if (item.kind === "bk") console.log(`  BK Loader: ${item.loader ?? "not configured (optional)"}`);
          if (item.path.length) console.log(`  PATH: ${item.path.join(":")}`);
        }
      }
      for (const item of installations) {
        console.log(`\n${item.kind} ${item.version}: ${item.status}`);
        if (action === "env") {
          console.log(`  Tools: ${item.tools}`);
          if (item.executable) console.log(`  Executable: ${item.executable}`);
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
  const tool = name && resources.isTool(name) ? resources.select(name, selected) : undefined;
  const family = name && !tool ? kind(name) : undefined;
  const sdkVersion = selected ?? manager.entries.find(sdk => sdk.kind === family && sdk.default)?.version;
  const sdk = !name || tool ? undefined : manager.sdk(name, sdkVersion ?? "");
  if (action === "setup" || action === "adopt") {
    if (!selected) throw new Error(`${action} requires an explicit catalog version`);
    if (tool) {
      if (action === "adopt" && !flags.path) throw new Error("adopt a tool with --path <installation-directory>");
      const item = await resources.setup(tool, flags.path as string | undefined);
      console.log(`${item.kind} ${item.version}: ready\nTools: ${item.root}`);
      return;
    }
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
    console.log(`${item.kind} ${item.version}: ${item.ready ? "ready" : "sources ready"}\nSDK: ${item.sdk}\nTools: ${item.tools}`);
    return;
  }
  const environment = tool ? await manager.toolEnvironment(tool)
    : sdk ? await manager.environment(sdk) : await manager.defaultEnvironment();
  if (action === "env") {
    if (!["bash", "zsh", "sh"].includes(String(flags.shell))) throw new Error("--shell must be bash, zsh, or sh");
    if (flags.json) throw new Error("--json and --shell cannot be combined");
    console.log(shellEnvironment(environment)); return;
  }
  if (action === "run" || action === "shell") {
    const command = action === "run" ? tail : [process.env.SHELL ?? "/bin/sh", "-i"];
    if (!command.length) throw new Error("run requires -- followed by a command");
    const child = Bun.spawn(command, { env: environment, stdin: "inherit", stdout: "inherit", stderr: "inherit" });
    const forward = (signal: NodeJS.Signals) => { try { child.kill(signal); } catch {} };
    const interrupt = () => forward("SIGINT");
    const terminate = () => forward("SIGTERM");
    process.on("SIGINT", interrupt);
    process.on("SIGTERM", terminate);
    try { process.exitCode = await child.exited; }
    finally { process.off("SIGINT", interrupt); process.off("SIGTERM", terminate); }
    return;
  }
  throw new Error(`Unknown command ${action}; run gizos --help`);
}

main().catch(error => {
  console.error(`gizos: ${error.message}`);
  process.exitCode = 1;
});
