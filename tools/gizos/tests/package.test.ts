import { expect, test } from "bun:test";
import { mkdtemp, readFile, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { build, releaseVersion } from "../build";
import { execute } from "../src/manager";
import { selectVersion } from "../src/store";

test("release version identifies the GizOS release batch", () => {
  expect(releaseVersion("20261030-012345", "0.1.0")).toBe("2026.10.30-r012345");
  expect(releaseVersion("local", "0.1.0")).toBe("0.1.0");
  expect(() => releaseVersion("20260230-120000", "0.1.0")).toThrow("Invalid");
  expect(() => releaseVersion("../outside", "0.1.0")).toThrow("batch");
});

test("public gizos alias targets npmjs and retains standalone SDK commands", async () => {
  const root = await mkdtemp(join(process.env.TEST_TMPDIR ?? tmpdir(), "gizos-public-package-"));
  try {
    const output = join(root, "package");
    const manifest = await build({ output, "package-name": "gizos", batch: "20261030-012345", version: "1.2.3-snapshot.4" });
    expect(manifest.name).toBe("gizos");
    expect(manifest.version).toBe("1.2.3-snapshot.4");
    expect(manifest.publishConfig).toEqual({ access: "public", registry: "https://registry.npmjs.org/" });
    expect(manifest.bin).toEqual({ gizos: "bin/gizos.js" });
    expect(manifest.scripts).toBeUndefined();
    expect(manifest.devDependencies).toBeUndefined();
    const entry = join(output, manifest.bin.gizos);
    expect((await execute([process.execPath, entry, "--version"], { cwd: "/" })).trim()).toBe(manifest.version);
    expect(await execute([process.execPath, entry, "--help"], { cwd: "/" })).toContain("from gizos and preserves SDKs");
    const environment = { ...process.env, GIZOS_DEVENV_HOME: join(root, "shared"), XDG_CONFIG_HOME: join(root, "config") };
    const result = JSON.parse(await execute([process.execPath, entry, "env", "bk", "--json"], { cwd: "/", env: environment }));
    expect(result.environments[0].status).toBe("not installed");
    await expect(build({ output, "package-name": "unexpected" })).rejects.toThrow("must be named gizos");
    for (const version of ["latest", "01.2.3", "1.2.3-01", "../outside"]) {
      await expect(build({ output, version })).rejects.toThrow("exact semantic version");
    }
  } finally { await rm(root, { recursive: true, force: true }); }
});

test("built npm entry runs from a separate directory without source checkout dependencies", async () => {
  const root = await mkdtemp(join(process.env.TEST_TMPDIR ?? tmpdir(), "gizos-package-"));
  try {
    const output = join(root, "package");
    const manifest = await build({ output, batch: "20261030-012345" });
    expect(manifest.name).toBe("@gizclaw/gizos");
    expect(manifest.publishConfig.registry).toBe("https://static-volc.gizclaw.com/npm/");
    expect(manifest.bin).toEqual({ gizos: "bin/gizos.js" });
    expect(manifest.version).toBe("2026.10.30-r012345");
    const entry = join(output, manifest.bin.gizos);
    expect((await stat(entry)).mode & 0o111).toBe(0o111);
    expect(await readFile(entry, "utf8")).toStartWith("#!/usr/bin/env bun");
    expect((await execute([process.execPath, entry, "--version"], { cwd: "/" })).trim()).toBe(manifest.version);
    const help = await execute([process.execPath, entry, "--help"], { cwd: "/" });
    expect(help).toContain("Install/open Gizloader desktop");
    expect(help).toContain("gizos install gizloader");
    expect(help).not.toContain("Tools: adb, bk-loader, h2loader");
    const environment = {
      ...process.env, GIZOS_DEVENV_HOME: join(root, "shared"), XDG_CONFIG_HOME: join(root, "config"),
    };
    await expect(execute([process.execPath, entry, "env", "bk-loader", "--archive", "/unused"], {
      cwd: "/", env: environment,
    })).rejects.toThrow("--archive is only supported");
    await expect(execute([process.execPath, entry, "setup", "esp_idf", "6.2", "--archive", "/unused"], {
      cwd: "/", env: environment,
    })).rejects.toThrow("--archive is only supported");
    await expect(execute([process.execPath, entry, "install", "esp_idf", "6.2"], {
      cwd: "/", env: environment,
    })).rejects.toThrow("install requires a tool");
    const wrongArchive = join(root, "wrong.pkg");
    await writeFile(wrongArchive, "wrong vendor release");
    await expect(execute([process.execPath, entry, "install", "bk_loader", "--archive", wrongArchive], {
      cwd: "/", env: environment,
    })).rejects.toThrow("checksum mismatch");
    const description = await execute([process.execPath, entry, "describe", "esp_idf"], {
      cwd: "/", env: environment,
    });
    expect(description).toContain("Espressif IoT Development Framework");
    expect(description).toContain("gizos config esp-idf GizClaw");
    const info = JSON.parse(await execute([process.execPath, entry, "env", "bk", "--json"], {
      cwd: "/", env: environment,
    }));
    expect(info.root).toBe(join(root, "shared"));
    expect(info.environments[0].status).toBe("not installed");
    const paths = { sdk: join(root, "external SDK"), tools: join(root, "external IDF tools") };
    await execute([
      process.execPath, entry, "config", "esp_idf", "6.2", "--sdk", paths.sdk, "--tools", paths.tools,
    ], { cwd: "/", env: environment });
    const configured = JSON.parse(await execute([process.execPath, entry, "env", "esp_idf", "6.2", "--json"], {
      cwd: "/", env: environment,
    }));
    expect(configured.environments[0]).toMatchObject({
      status: "not installed", sdk: paths.sdk, tools: paths.tools, configuredPaths: paths,
    });
    await selectVersion(environment.GIZOS_DEVENV_HOME, "esp-idf", "6.0.3");
    const versions = JSON.parse(await execute([process.execPath, entry, "env", "esp_idf", "--json"], {
      cwd: "/", env: environment,
    }));
    expect(versions.environments.find((item: any) => item.selected).version).toBe("6.0.3");
    const preview = JSON.parse(await execute([process.execPath, entry, "prune", "--dry-run", "--json"], {
      cwd: "/", env: environment,
    }));
    expect(preview).toEqual({ dryRun: true, remove: [], preserve: [] });
  } finally { await rm(root, { recursive: true, force: true }); }
});
