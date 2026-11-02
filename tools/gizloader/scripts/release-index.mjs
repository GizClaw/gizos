import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { lstat, readFile, readdir, writeFile } from "node:fs/promises";
import { basename, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { parseArgs } from "node:util";

export const INDEX_FILE = "gizloader-index.json";
export const CHECKSUMS_FILE = "SHA256SUMS";
export const PLATFORMS = ["darwin-arm64", "win32-x64"];
const FORMATS = { "darwin-arm64": "dmg", "win32-x64": "nsis" };
const SEMVER = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/;

function check(condition, message) {
  if (!condition) throw new Error(message);
}

function object(value, label) {
  check(value !== null && typeof value === "object" && !Array.isArray(value), `${label} must be an object`);
}

function keys(value, allowed, label) {
  object(value, label);
  check(Object.keys(value).every((key) => allowed.includes(key)), `${label} has unexpected fields`);
}

export function validateReleaseIdentity({ version, channel, commit }, publicOnly = false) {
  const match = typeof version === "string" && SEMVER.exec(version);
  check(match && match[0] === version && (!match[4] || match[4].split(".").every((part) => !/^\d+$/.test(part) || part === "0" || !part.startsWith("0"))), "version must be valid semver");
  check((publicOnly ? ["release", "snapshot"] : ["release", "snapshot", "build"]).includes(channel), "channel must be release or snapshot (build is allowed only for artifact metadata)");
  check(channel !== "release" || !match[4], "release version must not be a prerelease");
  check(typeof commit === "string" && commit.length === 40 && /^[0-9a-f]{40}$/.test(commit), "commit must be 40 lowercase hexadecimal characters");
}

export function artifactFilename(version, platform) {
  check(PLATFORMS.includes(platform), `Unsupported platform: ${platform}`);
  return platform === "darwin-arm64"
    ? `gizloader-${version}-macos-arm64.dmg`
    : `gizloader-${version}-windows-x64-setup.exe`;
}

export function metadataFilename(platform) {
  check(PLATFORMS.includes(platform), `Unsupported platform: ${platform}`);
  return `gizloader-${platform}.metadata.json`;
}

function signingState(signing, platform, channel) {
  keys(signing, ["status", "identity", "notarized"], "signing");
  check(["signed", "ad-hoc", "unsigned"].includes(signing.status), "signing.status must be signed, ad-hoc, or unsigned");
  if (signing.identity !== undefined) {
    check(typeof signing.identity === "string" && signing.identity.length > 0 && !/[\r\n\0]/.test(signing.identity), "signing.identity must be a nonempty single line");
  }
  if (signing.notarized !== undefined) check(typeof signing.notarized === "boolean", "signing.notarized must be boolean");
  if (signing.status !== "signed") {
    check(!signing.notarized && signing.identity === undefined, "Unsigned or ad-hoc artifacts cannot claim an identity or notarization");
  }
  if (platform === "win32-x64") {
    check(signing.status === "unsigned" && signing.notarized === undefined, "Windows installers must declare their current unsigned state");
  } else if (channel !== "build") {
    check(signing.status === "signed" && /^Developer ID Application: [^\r\n]+ \([A-Z0-9]{10}\)$/.test(signing.identity ?? "") && signing.notarized === true,
      "Public macOS artifacts require a notarized Developer ID Application signature; unsigned fallback is forbidden");
  }
}

function artifactRecord(artifact, version, channel) {
  keys(artifact, ["platform", "file", "sha256", "size", "format", "app", "signing"], "artifact");
  check(PLATFORMS.includes(artifact.platform), `Unsupported platform: ${artifact.platform}`);
  check(artifact.file === artifactFilename(version, artifact.platform), `Unexpected artifact filename: ${artifact.file}`);
  check(artifact.format === FORMATS[artifact.platform], `Unexpected format for ${artifact.platform}`);
  check(typeof artifact.sha256 === "string" && artifact.sha256.length === 64 && /^[0-9a-f]{64}$/.test(artifact.sha256), "artifact.sha256 must be 64 lowercase hexadecimal characters");
  check(Number.isSafeInteger(artifact.size) && artifact.size > 0, "artifact.size must be a positive safe integer");
  if (artifact.app !== undefined) {
    check(typeof artifact.app === "string" && artifact.app.length > 0 && basename(artifact.app) === artifact.app && !/[\\/\r\n\0]/.test(artifact.app) && artifact.app !== "." && artifact.app !== "..", "artifact.app must be a basename");
  }
  // A missing signing field must not erase the current Windows unsigned state or
  // let a public macOS build bypass the signing/notarization gate.
  signingState(artifact.signing, artifact.platform, channel);
}

function documentRecord(document, expected, count) {
  keys(document, ["schema", "version", "channel", "commit", "artifacts"], "metadata");
  check(document.schema === 1, "metadata.schema must be 1");
  validateReleaseIdentity(document);
  for (const field of ["version", "channel", "commit"]) {
    check(document[field] === expected[field], `Metadata ${field} does not match requested ${field}`);
  }
  check(Array.isArray(document.artifacts) && document.artifacts.length === count, `Expected exactly ${count} artifact${count === 1 ? "" : "s"}`);
  for (const artifact of document.artifacts) artifactRecord(artifact, document.version, document.channel);
}

async function regularFile(path) {
  const stat = await lstat(path);
  check(stat.isFile() && !stat.isSymbolicLink(), `Expected a regular file: ${path}`);
  return stat;
}

async function digest(path) {
  const stat = await regularFile(path);
  check(Number.isSafeInteger(stat.size) && stat.size > 0, `Artifact must be nonempty: ${path}`);
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(path)) hash.update(chunk);
  return { sha256: hash.digest("hex"), size: stat.size };
}

async function jsonFile(path) {
  await regularFile(path);
  return JSON.parse(await readFile(path, "utf8"));
}

function encode(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

/**
 * package.mjs calls this after copying and verifying its installer. The input
 * directory contains the exact filename returned by artifactFilename(). Public
 * macOS packaging must verify codesign, Gatekeeper, and stapling before claiming
 * {status: "signed", identity: "Developer ID Application: ...", notarized: true}.
 * This helper records evidence supplied by the native producer; assembly checks
 * it and independently rehashes every installer.
 */
export async function writeArtifactMetadata({ directory, version, channel, commit, platform, app, signing }) {
  validateReleaseIdentity({ version, channel, commit });
  const file = artifactFilename(version, platform);
  const artifact = { platform, file, ...await digest(join(directory, file)), format: FORMATS[platform] };
  if (app !== undefined) artifact.app = app;
  artifact.signing = signing;
  artifactRecord(artifact, version, channel);
  const metadata = { schema: 1, version, channel, commit, artifacts: [artifact] };
  const path = join(directory, metadataFilename(platform));
  await writeFile(path, encode(metadata));
  return { metadata, path };
}

async function verifyArtifact(directory, artifact) {
  const actual = await digest(join(directory, artifact.file));
  check(actual.size === artifact.size, `Size mismatch: ${artifact.file}`);
  check(actual.sha256 === artifact.sha256, `SHA-256 mismatch: ${artifact.file}`);
}

async function directoryContents(directory, version) {
  const allowed = new Set([
    ...PLATFORMS.map((platform) => artifactFilename(version, platform)),
    ...PLATFORMS.map(metadataFilename), INDEX_FILE, CHECKSUMS_FILE,
  ]);
  for (const entry of await readdir(directory)) {
    check(allowed.has(entry), `Unexpected release file: ${entry}`);
    await regularFile(join(directory, entry));
  }
}

async function checksumContents(directory, index) {
  const files = [...index.artifacts.map((artifact) => artifact.file), INDEX_FILE].sort();
  const lines = [];
  for (const file of files) lines.push(`${(await digest(join(directory, file))).sha256}  ${file}\n`);
  return lines.join("");
}

export async function assembleRelease({ directory, version, channel, commit }) {
  validateReleaseIdentity({ version, channel, commit }, true);
  await directoryContents(directory, version);
  const artifacts = [];
  for (const platform of PLATFORMS) {
    const metadata = await jsonFile(join(directory, metadataFilename(platform)));
    documentRecord(metadata, { version, channel, commit }, 1);
    check(metadata.artifacts[0].platform === platform, `Metadata platform does not match ${platform}`);
    await verifyArtifact(directory, metadata.artifacts[0]);
    artifacts.push(metadata.artifacts[0]);
  }
  const index = { schema: 1, version, channel, commit, artifacts };
  await writeFile(join(directory, INDEX_FILE), encode(index));
  await writeFile(join(directory, CHECKSUMS_FILE), await checksumContents(directory, index));
  return index;
}

export async function verifyRelease({ directory, version, channel, commit }) {
  validateReleaseIdentity({ version, channel, commit }, true);
  await directoryContents(directory, version);
  const index = await jsonFile(join(directory, INDEX_FILE));
  documentRecord(index, { version, channel, commit }, PLATFORMS.length);
  check(PLATFORMS.every((platform, position) => index.artifacts[position].platform === platform), "Index must include each expected platform exactly once in canonical order");
  for (const artifact of index.artifacts) await verifyArtifact(directory, artifact);
  await regularFile(join(directory, CHECKSUMS_FILE));
  check(await readFile(join(directory, CHECKSUMS_FILE), "utf8") === await checksumContents(directory, index), "SHA256SUMS does not match index and installers");
  return index;
}

async function main() {
  const command = process.argv[2];
  check(["artifact", "assemble", "verify"].includes(command), "Usage: release-index.mjs artifact|assemble|verify --directory DIR --version SEMVER --channel CHANNEL --commit SHA [--platform PLATFORM --app NAME --signing JSON]");
  const { values } = parseArgs({
    args: process.argv.slice(3),
    options: Object.fromEntries(["directory", "version", "channel", "commit", ...(command === "artifact" ? ["platform", "app", "signing"] : [])].map((key) => [key, { type: "string" }])),
    strict: true,
    allowPositionals: false,
  });
  check(values.directory, "--directory is required");
  const options = {
    directory: resolve(values.directory),
    version: values.version ?? process.env.GIZLOADER_VERSION,
    channel: values.channel ?? process.env.GIZLOADER_CHANNEL,
    commit: values.commit ?? process.env.GIZLOADER_COMMIT,
  };
  if (command === "artifact") {
    await writeArtifactMetadata({ ...options, platform: values.platform, app: values.app, signing: values.signing && JSON.parse(values.signing) });
  } else if (command === "assemble") {
    await assembleRelease(options);
  } else {
    await verifyRelease(options);
  }
  console.log(`Gizloader ${command}: ${options.version} (${options.channel})`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch((error) => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
