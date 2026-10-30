import { expect, test } from "bun:test";
import { mkdtemp, readFile, rm, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { build, releaseVersion } from "../build";
import { execute } from "../src/manager";

test("release version identifies the GizOS release batch", () => {
  expect(releaseVersion("20261030-012345", "0.1.0")).toBe("2026.10.30-r012345");
  expect(releaseVersion("local", "0.1.0")).toBe("0.1.0");
  expect(() => releaseVersion("20260230-120000", "0.1.0")).toThrow("Invalid");
  expect(() => releaseVersion("../outside", "0.1.0")).toThrow("batch");
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
    const info = JSON.parse(await execute([process.execPath, entry, "env", "bk", "--json"], {
      cwd: "/", env: { ...process.env, GIZOS_DEVENV_HOME: join(root, "shared") },
    }));
    expect(info.root).toBe(join(root, "shared"));
    expect(info.environments[0].status).toBe("not installed");
  } finally { await rm(root, { recursive: true, force: true }); }
});
