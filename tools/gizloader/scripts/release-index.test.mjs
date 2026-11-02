import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFile } from "node:child_process";
import { mkdtemp, readFile, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import test from "node:test";
import {
  artifactFilename, assembleRelease, CHECKSUMS_FILE, INDEX_FILE,
  metadataFilename, PLATFORMS, validateReleaseIdentity, verifyRelease, writeArtifactMetadata,
} from "./release-index.mjs";

const COMMIT = "a1".repeat(20);
const SIGNED = { status: "signed", identity: "Developer ID Application: Gizclaw Connect Limited (D82Y886BX5)", notarized: true };
const run = promisify(execFile);

async function fixture(t, overrides = {}) {
  const directory = await mkdtemp(join(tmpdir(), "gizloader-index-"));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const options = { directory, version: "0.1.0", channel: "release", commit: COMMIT, ...overrides };
  for (const platform of PLATFORMS) {
    await writeFile(join(directory, artifactFilename(options.version, platform)), `fixture installer for ${platform}\n`);
    await writeArtifactMetadata({ ...options, platform, app: platform === "darwin-arm64" ? "Gizloader.app" : "gizloader.exe", signing: platform === "darwin-arm64" ? SIGNED : { status: "unsigned" } });
  }
  return options;
}

async function editMetadata(options, platform, change) {
  const path = join(options.directory, metadataFilename(platform));
  const metadata = JSON.parse(await readFile(path, "utf8"));
  change(metadata);
  await writeFile(path, JSON.stringify(metadata));
}

test("assemble and verify exact installers, provenance, signing, and checksums", async (t) => {
  const options = await fixture(t);
  const index = await assembleRelease(options);
  assert.deepEqual(index, await verifyRelease(options));
  assert.equal(index.schema, 1);
  assert.equal(index.commit, COMMIT);
  assert.deepEqual(index.artifacts.map((artifact) => artifact.platform), PLATFORMS);
  assert.deepEqual(index.artifacts[0].signing, SIGNED);
  assert.deepEqual(index.artifacts[1].signing, { status: "unsigned" });
  const checksums = await readFile(join(options.directory, CHECKSUMS_FILE), "utf8");
  const expectedFiles = [...PLATFORMS.map((platform) => artifactFilename(options.version, platform)), INDEX_FILE].sort();
  assert.deepEqual(checksums.trim().split("\n").map((line) => line.slice(66)), expectedFiles);
  for (const file of expectedFiles) {
    const expected = createHash("sha256").update(await readFile(join(options.directory, file))).digest("hex");
    assert.ok(checksums.includes(`${expected}  ${file}\n`));
  }
  // Published bundles carry the index and installers, not private assembly inputs.
  for (const platform of PLATFORMS) await rm(join(options.directory, metadataFilename(platform)));
  assert.deepEqual(await verifyRelease(options), index);
});

test("snapshot semver is preserved and build metadata cannot be publicly assembled", async (t) => {
  const options = await fixture(t, { channel: "snapshot", version: "0.1.0-snapshot.42" });
  assert.equal((await assembleRelease(options)).version, "0.1.0-snapshot.42");
  await assert.rejects(assembleRelease({ ...options, channel: "build" }), /channel/);
});

test("reject invalid semver, release prereleases, and malformed commits", async (t) => {
  const options = await fixture(t);
  for (const version of ["01.1.0", "0.01.0", "0.1", "0.1.0-x..y", "0.1.0-snapshot.01", "../0.1.0", "0.1.0\n"]) {
    assert.throws(() => validateReleaseIdentity({ ...options, version }), /version/);
    await assert.rejects(assembleRelease({ ...options, version }), /version/);
  }
  await assert.rejects(assembleRelease({ ...options, version: "0.1.0-rc.1" }), /prerelease/);
  for (const commit of ["abc123", "g".repeat(40), "A".repeat(40), `${COMMIT}\n`]) {
    assert.throws(() => validateReleaseIdentity({ ...options, commit }), /commit/);
    await assert.rejects(assembleRelease({ ...options, commit }), /commit/);
  }
});

test("metadata writer returns a fragment and allows artifact-only ad-hoc builds", async (t) => {
  const options = await fixture(t, { channel: "build" });
  const result = await writeArtifactMetadata({ ...options, platform: "darwin-arm64", signing: { status: "ad-hoc", notarized: false } });
  assert.equal(result.path, join(options.directory, metadataFilename("darwin-arm64")));
  assert.equal(result.metadata.artifacts.length, 1);
  assert.deepEqual(result.metadata.artifacts[0].signing, { status: "ad-hoc", notarized: false });
});

test("public metadata fails closed for missing or unsigned macOS claims", async (t) => {
  const options = await fixture(t);
  for (const signing of [undefined, { status: "unsigned" }, { status: "ad-hoc" }, { status: "signed", identity: "-", notarized: true }, { ...SIGNED, notarized: false }, { status: "signed", notarized: true }, { ...SIGNED, unexpected: true }]) {
    await assert.rejects(writeArtifactMetadata({ ...options, platform: "darwin-arm64", signing }), /signing|signature|Unsigned/);
  }
  await assert.rejects(writeArtifactMetadata({ ...options, platform: "win32-x64", signing: { status: "signed" } }), /Windows/);
});

test("assembly checks signing evidence even when fragments were not written by the helper", async (t) => {
  const options = await fixture(t);
  await editMetadata(options, "darwin-arm64", (metadata) => { delete metadata.artifacts[0].signing; });
  await assert.rejects(assembleRelease(options), /signing/);
});

test("reject tampered artifact hashes and sizes", async (t) => {
  const options = await fixture(t);
  const path = join(options.directory, artifactFilename(options.version, "win32-x64"));
  const original = await readFile(path);
  const replacement = Buffer.from(original);
  replacement[0] ^= 1;
  await writeFile(path, replacement);
  await assert.rejects(assembleRelease(options), /SHA-256 mismatch/);
  await writeFile(path, Buffer.concat([original, Buffer.from("extra")]));
  await assert.rejects(assembleRelease(options), /Size mismatch/);
});

test("require both unique platforms and reject inconsistent fragment provenance", async (t) => {
  for (const field of ["version", "channel", "commit"]) {
    const options = await fixture(t);
    await editMetadata(options, "win32-x64", (metadata) => {
      metadata[field] = { version: "0.2.0", channel: "snapshot", commit: "b".repeat(40) }[field];
    });
    await assert.rejects(assembleRelease(options), new RegExp(`Metadata ${field}`));
  }
  const options = await fixture(t);
  await rm(join(options.directory, metadataFilename("win32-x64")));
  await assert.rejects(assembleRelease(options), /ENOENT/);
  await writeFile(join(options.directory, metadataFilename("win32-x64")), await readFile(join(options.directory, metadataFilename("darwin-arm64"))));
  await assert.rejects(assembleRelease(options), /platform/);
});

test("reject unexpected artifact names, formats, and extra metadata fields", async (t) => {
  for (const [field, value, pattern] of [["file", "../installer.exe", /filename/], ["format", "zip", /format/], ["size", 0, /positive/], ["sha256", "bad", /sha256/], ["app", "..\\outside.exe", /basename/], ["unexpected", true, /unexpected/]]) {
    const options = await fixture(t);
    await editMetadata(options, "win32-x64", (metadata) => { metadata.artifacts[0][field] = value; });
    await assert.rejects(assembleRelease(options), pattern);
  }
});

test("reject stray files, directories, empty installers, and symlink inputs", async (t) => {
  const options = await fixture(t);
  await writeFile(join(options.directory, "gizloader-old.dmg"), "stale installer");
  await assert.rejects(assembleRelease(options), /Unexpected release file/);
  await rm(join(options.directory, "gizloader-old.dmg"));
  const installer = join(options.directory, artifactFilename(options.version, "win32-x64"));
  await writeFile(installer, "");
  await assert.rejects(assembleRelease(options), /nonempty/);
  await rm(installer);
  await symlink(join(options.directory, artifactFilename(options.version, "darwin-arm64")), installer);
  await assert.rejects(assembleRelease(options), /regular file/);
});

test("verification detects modified index/checksums and duplicate platform entries", async (t) => {
  const options = await fixture(t);
  const index = await assembleRelease(options);
  await writeFile(join(options.directory, INDEX_FILE), `${JSON.stringify(index)}\n`);
  await assert.rejects(verifyRelease(options), /SHA256SUMS/);
  await assembleRelease(options);
  await writeFile(join(options.directory, CHECKSUMS_FILE), "");
  await assert.rejects(verifyRelease(options), /SHA256SUMS/);
  index.artifacts[1] = index.artifacts[0];
  await writeFile(join(options.directory, INDEX_FILE), JSON.stringify(index));
  await assert.rejects(verifyRelease(options), /each expected platform/);
});

test("CLI assembles/verifies fragments and rejects unknown options", async (t) => {
  const options = await fixture(t);
  const script = fileURLToPath(new URL("./release-index.mjs", import.meta.url));
  const args = ["--directory", options.directory, "--version", options.version, "--channel", options.channel, "--commit", options.commit];
  assert.match((await run(process.execPath, [script, "assemble", ...args])).stdout, /Gizloader assemble/);
  assert.match((await run(process.execPath, [script, "verify", ...args])).stdout, /Gizloader verify/);
  const env = { ...process.env, GIZLOADER_VERSION: options.version, GIZLOADER_CHANNEL: options.channel, GIZLOADER_COMMIT: options.commit };
  assert.match((await run(process.execPath, [script, "verify", "--directory", options.directory], { env })).stdout, /Gizloader verify/);
  assert.match((await run(process.execPath, [script, "artifact", ...args, "--platform", "win32-x64", "--app", "gizloader.exe", "--signing", '{"status":"unsigned"}'])).stdout, /Gizloader artifact/);
  await assert.rejects(run(process.execPath, [script, "assemble", ...args, "--typo", "value"]), /Unknown option/);
});
