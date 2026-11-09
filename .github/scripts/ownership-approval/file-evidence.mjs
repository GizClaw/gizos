import {execFile} from "node:child_process";
import {mkdtemp, rm} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {promisify} from "node:util";

import {validateSha} from "../merge-eligibility-status/common.mjs";
import {normalizePath, PolicyError} from "./common.mjs";

const execFileAsync = promisify(execFile);
const API_FILE_LIMIT = 3000;

export function assertCompleteFiles(files, expectedCount) {
  if (
    !Number.isSafeInteger(expectedCount) ||
    expectedCount < 1 ||
    files.length !== expectedCount ||
    new Set(files.map((file) => file.filename)).size !== files.length
  ) {
    throw new PolicyError(
      `changed-file evidence is incomplete: expected ${expectedCount}, received ${files.length}`,
    );
  }
  return files;
}

function gitPath(path) {
  if (normalizePath(path) !== path) {
    throw new PolicyError("Git evidence contains a non-relative changed path");
  }
  return path;
}

export function parseGitFiles(output, expectedCount) {
  let text;
  try {
    text = new TextDecoder("utf-8", {fatal: true}).decode(output);
  } catch {
    throw new PolicyError("Git changed-file evidence is not valid UTF-8");
  }
  if (!text.endsWith("\0")) {
    throw new PolicyError("Git changed-file evidence is not NUL-terminated");
  }
  const fields = text.slice(0, -1).split("\0");
  const files = [];
  const statuses = {A: "added", D: "removed", M: "modified", T: "changed"};
  for (let index = 0; index < fields.length; ) {
    const status = fields[index++];
    const source = gitPath(fields[index++]);
    if (/^R(?:100|0[0-9]{2})$/.test(status)) {
      files.push({
        status: "renamed",
        filename: gitPath(fields[index++]),
        previous_filename: source,
      });
    } else if (Object.hasOwn(statuses, status)) {
      files.push({status: statuses[status], filename: source});
    } else {
      throw new PolicyError(`unsupported Git changed-file status: ${status}`);
    }
  }
  return assertCompleteFiles(files, expectedCount);
}

function gitEnvironment(extraConfig = []) {
  // Ignore caller Git configuration, replacement objects, hooks and credentials.
  const env = Object.fromEntries(
    Object.entries(process.env).filter(
      ([key]) => !key.startsWith("GIT_") && key !== "GITHUB_TOKEN",
    ),
  );
  Object.assign(env, {
    GIT_CONFIG_NOSYSTEM: "1",
    GIT_CONFIG_GLOBAL: "/dev/null",
    GIT_ATTR_NOSYSTEM: "1",
    GIT_NO_REPLACE_OBJECTS: "1",
    GIT_TERMINAL_PROMPT: "0",
  });
  const config = [
    ["core.hooksPath", "/dev/null"],
    ["core.attributesFile", "/dev/null"],
    ["credential.helper", ""],
    ["protocol.allow", "never"],
    ["protocol.https.allow", "always"],
    ["http.followRedirects", "false"],
    ...extraConfig,
  ];
  env.GIT_CONFIG_COUNT = String(config.length);
  for (const [index, [key, value]] of config.entries()) {
    env[`GIT_CONFIG_KEY_${index}`] = key;
    env[`GIT_CONFIG_VALUE_${index}`] = value;
  }
  return env;
}

async function git(directory, args, extraConfig) {
  try {
    const {stdout} = await execFileAsync("git", args, {
      cwd: directory,
      env: gitEnvironment(extraConfig),
      encoding: "buffer",
      maxBuffer: 64 * 1024 * 1024,
      timeout: 120000,
    });
    return stdout;
  } catch (error) {
    // Never include subprocess diagnostics that might contain credentials.
    throw new PolicyError(
      `Git ${args[0]} failed while collecting changed-file evidence (${error.code ?? "terminated"})`,
    );
  }
}

function repositoryUrl(repository, serverUrl) {
  if (!/^[a-z0-9_.-]+\/[a-z0-9_.-]+$/i.test(repository)) {
    throw new PolicyError("invalid trusted repository identity");
  }
  const server = new URL(serverUrl);
  if (
    server.protocol !== "https:" ||
    server.username ||
    server.password ||
    server.search ||
    server.hash ||
    server.pathname !== "/"
  ) {
    throw new PolicyError("trusted Git server must be an HTTPS origin");
  }
  return new URL(`${repository}.git`, server).href;
}

export async function trustedCheckoutSha() {
  return validateSha(
    (await git(process.cwd(), ["rev-parse", "--verify", "HEAD^{commit}"]))
      .toString("utf8").trim(),
    "trusted checkout SHA",
  );
}

export async function gitChangedFiles({
  repository,
  serverUrl,
  token,
  baseSha,
  headSha,
  expectedCount,
}) {
  validateSha(baseSha, "Git evidence base SHA");
  validateSha(headSha, "Git evidence head SHA");
  // Fetch only from the trusted base repository, including for fork PRs.
  const url = repositoryUrl(repository, serverUrl);
  const directory = await mkdtemp(join(tmpdir(), "ownership-git-evidence-"));
  try {
    await git(directory, ["init", "--bare", "--template="]);
    await git(
      directory,
      [
        "fetch",
        "--no-tags",
        "--no-recurse-submodules",
        "--no-write-fetch-head",
        "--",
        url,
        baseSha,
        headSha,
      ],
      token ? [[
        `http.${url}.extraHeader`,
        `Authorization: Basic ${Buffer.from(`x-access-token:${token}`).toString("base64")}`,
      ]] : [],
    );
    for (const sha of [baseSha, headSha]) {
      const received = (
        await git(directory, ["rev-parse", "--verify", `${sha}^{commit}`])
      ).toString("utf8").trim();
      if (received !== sha) {
        throw new PolicyError("fetched Git commit identity does not match the PR");
      }
    }
    const mergeBases = (
      await git(directory, ["merge-base", "--all", baseSha, headSha])
    ).toString("utf8").trim().split("\n");
    if (mergeBases.length !== 1) {
      throw new PolicyError("Git file evidence requires exactly one merge base");
    }
    const mergeBaseSha = validateSha(mergeBases[0], "Git evidence merge-base SHA");
    // Bound similarity matching, never the file list. If GitHub's rename count
    // differs, the exact changed_files check below still rejects the evidence.
    const output = await git(directory, [
      "diff",
      "--name-status",
      "-z",
      "--no-ext-diff",
      "--no-textconv",
      "--no-relative",
      "--ignore-submodules=none",
      "--no-renames",
      "--find-renames=50%",
      "-l1000",
      mergeBaseSha,
      headSha,
      "--",
    ]);
    return parseGitFiles(output, expectedCount);
  } finally {
    await rm(directory, {recursive: true, force: true});
  }
}

export async function changedFileEvidence(pullRequest, options, listFiles) {
  const expectedCount = pullRequest.changed_files;
  const files = expectedCount > API_FILE_LIMIT
    ? await gitChangedFiles({
        ...options,
        baseSha: pullRequest.base.sha,
        headSha: pullRequest.head.sha,
        expectedCount,
      })
    : await listFiles();
  return assertCompleteFiles(files, expectedCount);
}
