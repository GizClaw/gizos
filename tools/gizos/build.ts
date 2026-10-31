import { mkdir, readFile, writeFile, chmod, copyFile } from "node:fs/promises";
import { join, resolve } from "node:path";

export function releaseVersion(batch: string, fallback: string): string {
  if (batch === "bazel-release-batch" || batch === "local") return fallback;
  const match = /^(\d{4})(\d{2})(\d{2})-(\d{6})$/.exec(batch);
  if (!match) throw new Error("Release batch must be YYYYMMDD-HHMMSS");
  const timestamp = `${match[1]}-${match[2]}-${match[3]}T${match[4].slice(0, 2)}:${match[4].slice(2, 4)}:${match[4].slice(4)}Z`;
  const date = new Date(timestamp);
  if (Number.isNaN(date.getTime()) || date.toISOString().slice(0, 19) !== timestamp.slice(0, 19)) {
    throw new Error("Invalid release timestamp");
  }
  return `${Number(match[1])}.${Number(match[2])}.${Number(match[3])}-r${match[4]}`;
}

export async function build(options: Record<string, string>) {
  const base = import.meta.dir;
  const output = resolve(options.output ?? join(base, "dist"));
  const source = options["package-json"] ?? join(base, "package.json");
  const manifest = JSON.parse(await readFile(source, "utf8"));
  manifest.version = releaseVersion(options.batch ?? "local", manifest.version);
  // Build/package dependencies belong to the source checkout, never the CLI.
  delete manifest.scripts;
  delete manifest.devDependencies;
  await mkdir(join(output, "bin"), { recursive: true });
  const result = await Bun.build({
    entrypoints: [resolve(options.entry ?? join(base, "src/cli.ts"))],
    target: "bun",
    define: { GIZOS_VERSION: JSON.stringify(manifest.version) },
  });
  if (!result.success) throw new Error(result.logs.map(String).join("\n"));
  const javascript = await result.outputs[0].text();
  const entry = join(output, "bin/gizos.js");
  await writeFile(entry, javascript.startsWith("#!") ? javascript : "#!/usr/bin/env bun\n" + javascript);
  await chmod(entry, 0o755);
  await copyFile(options.readme ?? join(base, "README.md"), join(output, "README.md"));
  await copyFile(options.license ?? join(base, "../../LICENSE"), join(output, "LICENSE"));
  const json = JSON.stringify(manifest, null, 2) + "\n";
  await writeFile(join(output, "package.json"), json);
  if (options.manifest) await writeFile(options.manifest, json);
  return manifest;
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const options: Record<string, string> = {};
  for (let index = 0; index < args.length; index += 2) {
    if (!args[index].startsWith("--") || !args[index + 1]) throw new Error("Expected --option value");
    options[args[index].slice(2)] = args[index + 1];
  }
  await build(options);
}
