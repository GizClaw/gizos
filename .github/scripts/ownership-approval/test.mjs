import assert from "node:assert/strict";
import {spawn, execFileSync} from "node:child_process";
import {mkdir, mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {createServer} from "node:http";
import {tmpdir} from "node:os";
import {dirname, resolve} from "node:path";
import test from "node:test";
import {fileURLToPath} from "node:url";

import {
  changedPaths,
  evaluateOwnership,
  formatSummary,
  ownersForPath,
  parseCodeowners,
  PolicyError,
} from "./common.mjs";
import {assertCompleteFiles, gitChangedFiles, parseGitFiles} from "./file-evidence.mjs";

const HEAD = "0123456789abcdef0123456789abcdef01234567";
const SCRIPT_DIRECTORY = dirname(fileURLToPath(import.meta.url));
const REPOSITORY_ROOT = resolve(SCRIPT_DIRECTORY, "../../..");
const RUN_SCRIPT = resolve(SCRIPT_DIRECTORY, "run.mjs");
const OWNERSHIP_WORKFLOW = resolve(
  REPOSITORY_ROOT,
  ".github/workflows/ownership-approval.yml",
);
const CODEOWNERS = `
* @idy
/boards/example_devkit/ @Sid9017
/native_component_src/example_chip/ @Sid9017
`;

function file(filename, status = "modified", previousFilename) {
  return {
    filename,
    status,
    previous_filename: previousFilename,
  };
}

function review(login, state = "APPROVED", commitId = HEAD, id = 1) {
  return {
    id,
    user: {login},
    state,
    commit_id: commitId,
    submitted_at: `2026-08-04T00:00:${String(id).padStart(2, "0")}Z`,
  };
}

async function runPublishedCheck({
  files = [],
  changedFilesCount = files.length,
  reviews = [],
  reviewsAfterGit = null,
  author = "idy",
  authorPermission = "read",
  eventActor = author,
  eventKind = "pull_request_target",
  pullRequestFailure = false,
  pullRequestUpdates = [],
  eventUpdate = {},
  runUrl = "https://github.example/actions/runs/1",
  latestTargetUrl = runUrl,
  statusHistory = null,
  statusPublicationFailure = false,
  headRepository = "GizClaw/gizos",
  headSha = HEAD,
  repositoryRoot = REPOSITORY_ROOT,
  gitRemote = null,
  gitFailure = null,
  gitEnvironment = {},
}) {
  const baseSha = execFileSync("git", ["rev-parse", "HEAD"], {
    cwd: repositoryRoot,
    encoding: "utf8",
  }).trim();
  const pullRequest = {
    number: 1,
    state: "open",
    changed_files: changedFilesCount,
    user: {login: author},
    head: {sha: headSha, repo: {full_name: headRepository}},
    base: {sha: baseSha, ref: "main", repo: {full_name: "GizClaw/gizos"}},
  };
  let pullRequestReads = 0;
  const statusSha = eventUpdate.head?.sha ?? headSha;
  const requests = [];
  const server = createServer(async (request, response) => {
    const chunks = [];
    for await (const chunk of request) {
      chunks.push(chunk);
    }
    const bodyText = Buffer.concat(chunks).toString("utf8");
    const body = bodyText ? JSON.parse(bodyText) : null;
    const receivedRequest = {method: request.method, url: request.url, body};
    requests.push(receivedRequest);

    let payload;
    if (request.url === "/repos/GizClaw/gizos/pulls/1") {
      if (pullRequestFailure) {
        response.writeHead(500, {"Content-Type": "application/json"});
        response.end(JSON.stringify({message: "fixture failure"}));
        return;
      }
      payload = {...pullRequest, ...pullRequestUpdates[pullRequestReads++]};
    } else if (
      request.method === "GET" &&
      request.url ===
        `/repos/GizClaw/gizos/collaborators/${encodeURIComponent(author)}/permission`
    ) {
      payload = {permission: authorPermission};
    } else if (
      request.method === "GET" &&
      request.url ===
        `/repos/GizClaw/gizos/commits/${statusSha}/statuses?per_page=100`
    ) {
      payload = statusHistory ?? [
        {
          context: "Ownership eligibility",
          state: "pending",
          target_url: latestTargetUrl,
        },
      ];
    } else if (
      request.method === "POST" &&
      request.url === `/repos/GizClaw/gizos/statuses/${statusSha}`
    ) {
      if (statusPublicationFailure) {
        response.writeHead(500, {"Content-Type": "application/json"});
        response.end(JSON.stringify({message: "status publication failure"}));
        return;
      }
      statusHistory?.unshift(body);
      payload = {id: requests.length};
    } else if (
      request.method === "POST" &&
      request.url === "/repos/GizClaw/gizos/actions/workflows/ci.yml/dispatches"
    ) {
      payload = null;
    } else if (
      request.method === "POST" &&
      request.url ===
        "/repos/GizClaw/gizos/actions/workflows/openai-pr-review.yml/dispatches"
    ) {
      payload = null;
    } else if (request.url.startsWith("/repos/GizClaw/gizos/pulls/1/files?")) {
      const page = Number(new URL(request.url, "http://fixture").searchParams.get("page"));
      payload = files.slice((page - 1) * 100, Math.min(page * 100, 3000));
    } else if (request.url.startsWith("/repos/GizClaw/gizos/pulls/1/reviews?")) {
      const page = Number(new URL(request.url, "http://fixture").searchParams.get("page"));
      const gitComplete = await readFile(gitEvidencePath, "utf8").then(
        () => true,
        (error) => { if (error.code !== "ENOENT") throw error; return false; },
      );
      receivedRequest.gitEvidenceComplete = gitComplete;
      const liveReviews = gitComplete && reviewsAfterGit !== null ? reviewsAfterGit : reviews;
      payload = liveReviews.slice((page - 1) * 100, page * 100);
    } else {
      response.writeHead(404, {"Content-Type": "application/json"});
      response.end(JSON.stringify({message: `unhandled ${request.method} ${request.url}`}));
      return;
    }
    response.writeHead(200, {"Content-Type": "application/json"});
    response.end(JSON.stringify(payload));
  });
  await new Promise((resolveListen) => server.listen(0, "127.0.0.1", resolveListen));
  const address = server.address();
  const temporaryDirectory = await mkdtemp(`${tmpdir()}/ownership-approval-`);
  const eventPath = resolve(temporaryDirectory, "event.json");
  const summaryPath = resolve(temporaryDirectory, "summary.md");
  const outputPath = resolve(temporaryDirectory, "output.txt");
  const gitCommandsPath = resolve(temporaryDirectory, "git-commands.jsonl");
  const gitEvidencePath = resolve(temporaryDirectory, "git-complete");
  const childEnvironment = {...process.env, ...gitEnvironment};
  if (gitRemote) {
    const binDirectory = resolve(temporaryDirectory, "bin");
    await mkdir(binDirectory);
    const realGit = execFileSync("which", ["git"], {encoding: "utf8"}).trim();
    // Exercise real Git with only the network transport redirected to a fixture.
    await writeFile(resolve(binDirectory, "git"), `#!${process.execPath}
const {appendFileSync, writeFileSync} = require("node:fs");
const {spawnSync} = require("node:child_process");
const args = process.argv.slice(2);
appendFileSync(${JSON.stringify(gitCommandsPath)}, JSON.stringify({args, cwd: process.cwd()}) + "\\n");
if (args[0] === ${JSON.stringify(gitFailure)}) process.exit(7);
if (args[0] === "fetch") {
  const separator = args.indexOf("--");
  if (args[separator + 1] !== "https://github.com/GizClaw/gizos.git") process.exit(8);
  args[separator + 1] = ${JSON.stringify(gitRemote)};
  args.unshift("-c", "protocol.file.allow=always");
}
const result = spawnSync(${JSON.stringify(realGit)}, args, {stdio: "inherit"});
if (args[0] === "diff" && result.status === 0) writeFileSync(${JSON.stringify(gitEvidencePath)}, "complete");
process.exit(result.status ?? 1);
`, {mode: 0o755});
    childEnvironment.PATH = `${binDirectory}:${process.env.PATH}`;
  }
  const event =
    eventKind === "workflow_run"
      ? {
          workflow_run: {
            name: "Ownership Approval Review Request",
            event: "pull_request_review",
            conclusion: "success",
            pull_requests: [{...pullRequest}],
          },
        }
      : {
          action: "synchronize",
          sender: {login: eventActor},
          pull_request: {
            ...pullRequest,
          },
        };
  if (eventKind === "workflow_run") {
    Object.assign(event.workflow_run.pull_requests[0], eventUpdate);
  } else {
    Object.assign(event.pull_request, eventUpdate);
  }
  await writeFile(eventPath, JSON.stringify(event));

  try {
    const child = spawn(process.execPath, [RUN_SCRIPT], {
      cwd: repositoryRoot,
      env: {
        ...childEnvironment,
        GITHUB_API_URL: `http://127.0.0.1:${address.port}`,
        GITHUB_EVENT_PATH: eventPath,
        GITHUB_OUTPUT: outputPath,
        GITHUB_REPOSITORY: "GizClaw/gizos",
        GITHUB_RUN_URL: runUrl,
        GITHUB_STEP_SUMMARY: summaryPath,
        GITHUB_TOKEN: "test-token",
      },
      stdio: ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    child.stdout.on("data", (chunk) => {
      stdout += chunk;
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk;
    });
    const exitCode = await new Promise((resolveExit) =>
      child.on("close", resolveExit),
    );
    const summary = await readFile(summaryPath, "utf8");
    const output = await readFile(outputPath, "utf8");
    const gitCommands = gitRemote
      ? (await readFile(gitCommandsPath, "utf8")).trim().split("\n").map((line) => JSON.parse(line))
      : [];
    return {exitCode, requests, stdout, stderr, summary, output, gitCommands};
  } finally {
    await new Promise((resolveClose) => server.close(resolveClose));
    await rm(temporaryDirectory, {recursive: true, force: true});
  }
}

test("runner keeps merge ownership independent from CI", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    author: "contributor",
    authorPermission: "write",
  });
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  const statuses = result.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.equal(statuses.at(-1).body.state, "failure");
  assert.match(result.output, /eligible=false/);
  assert.match(result.output, new RegExp(`head_sha=${HEAD}`));
});

test("later matching directory rule overrides the default owner", () => {
  const rules = parseCodeowners(CODEOWNERS);
  assert.deepEqual(ownersForPath(rules, "README.md").directOwners, ["idy"]);
  assert.deepEqual(
    ownersForPath(rules, "boards/example_devkit/src/main.c").directOwners,
    ["sid9017"],
  );
});

test("root bare-directory patterns own descendants without a trailing slash", () => {
  const rules = parseCodeowners("* @idy\n/docs @Sid9017\n");
  assert.deepEqual(ownersForPath(rules, "docs/guide.md").directOwners, [
    "sid9017",
  ]);
  assert.deepEqual(
    ownersForPath(rules, "archive/docs/guide.md").directOwners,
    ["idy"],
  );
});

test("relative bare-directory patterns own matching descendants at any depth", () => {
  const rules = parseCodeowners("* @idy\ndocs @Sid9017\n");
  assert.deepEqual(ownersForPath(rules, "docs/guide.md").directOwners, [
    "sid9017",
  ]);
  assert.deepEqual(
    ownersForPath(rules, "archive/docs/guide.md").directOwners,
    ["sid9017"],
  );
});

test("wildcard final segments do not acquire nested descendants", () => {
  const rules = parseCodeowners("* @idy\n/docs/* @Sid9017\n");
  assert.deepEqual(ownersForPath(rules, "docs/generated.json").directOwners, [
    "sid9017",
  ]);
  assert.deepEqual(
    ownersForPath(rules, "docs/generated/output/data.json").directOwners,
    ["idy"],
  );
});

test("question mark matches exactly one non-slash character", () => {
  const rules = parseCodeowners("* @idy\n/config?.yml @release\n");
  assert.deepEqual(ownersForPath(rules, "config1.yml").directOwners, [
    "release",
  ]);
  assert.deepEqual(ownersForPath(rules, "config12.yml").directOwners, ["idy"]);
});

test("trailing slash and bare literal directory rules are equivalent", () => {
  const bare = parseCodeowners("* @idy\n/docs @Sid9017\n");
  const trailing = parseCodeowners("* @idy\n/docs/ @Sid9017\n");
  assert.deepEqual(
    ownersForPath(bare, "docs/design/contract.md").directOwners,
    ownersForPath(trailing, "docs/design/contract.md").directOwners,
  );
});

test("double-star patterns match zero or more directories", () => {
  const rules = parseCodeowners("* @idy\n**/generated/*.c @buildbot\n");
  assert.deepEqual(ownersForPath(rules, "generated/a.c").directOwners, [
    "buildbot",
  ]);
  assert.deepEqual(
    ownersForPath(rules, "components/generated/a.c").directOwners,
    ["buildbot"],
  );
});

test("globstar with a literal final segment owns directory descendants", () => {
  const rules = parseCodeowners("* @idy\n**/logs @operations\n");
  assert.deepEqual(
    ownersForPath(rules, "deeply/nested/logs/archive/output.txt").directOwners,
    ["operations"],
  );
});

test("inline comments do not become owner tokens", () => {
  const rules = parseCodeowners("* @idy\n*.js @frontend # JavaScript owner\n");
  assert.deepEqual(ownersForPath(rules, "src/app.js").directOwners, [
    "frontend",
  ]);
  assert.deepEqual(ownersForPath(rules, "src/app.c").directOwners, ["idy"]);
});

test("author directly owning every path passes without reviews", () => {
  const result = evaluateOwnership({
    rules: parseCodeowners(CODEOWNERS),
    files: [file("README.md"), file("libs/runtime/src/runtime.c")],
    reviews: [],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(result.success, true);
  assert.equal(result.selfOwned.length, 2);
  assert.deepEqual(result.blockers, []);
});

test("fallback owner cannot bypass a bare-directory ownership rule", () => {
  const result = evaluateOwnership({
    rules: parseCodeowners("* @idy\n/docs @Sid9017\n"),
    files: [file("docs/security/policy.md")],
    reviews: [],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(result.success, false);
  assert.match(result.blockers[0], /@sid9017/);
});

test("later rules and any matching direct owner satisfy multiple-owner rules", () => {
  const rules = parseCodeowners(
    "# default\n* @idy\n/docs @alice @bob\n/docs/private @security\n",
  );
  const publicResult = evaluateOwnership({
    rules,
    files: [file("docs/public.md")],
    reviews: [review("bob")],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(publicResult.success, true);
  const privateResult = evaluateOwnership({
    rules,
    files: [file("docs/private/key.md")],
    reviews: [review("bob")],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(privateResult.success, false);
  assert.match(privateResult.blockers[0], /@security/);
});

test("cross-owner path requires its direct owner's approval", () => {
  const inputs = {
    rules: parseCodeowners(CODEOWNERS),
    files: [file("README.md"), file("native_component_src/example_chip/driver.c")],
    author: "idy",
    headSha: HEAD,
  };
  const blocked = evaluateOwnership({...inputs, reviews: []});
  assert.equal(blocked.success, false);
  assert.match(blocked.blockers[0], /@sid9017/);

  const approved = evaluateOwnership({
    ...inputs,
    reviews: [review("Sid9017")],
  });
  assert.equal(approved.success, true);
  assert.equal(approved.approved.length, 1);
  assert.deepEqual(approved.approvalApprovers, ["sid9017"]);
});

test("stale, author, and unrelated approvals do not satisfy the policy", () => {
  const result = evaluateOwnership({
    rules: parseCodeowners(CODEOWNERS),
    files: [file("native_component_src/example_chip/driver.c")],
    reviews: [
      review("Sid9017", "APPROVED", "f".repeat(40), 1),
      review("idy", "APPROVED", HEAD, 2),
      review("someone-else", "APPROVED", HEAD, 3),
    ],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(result.success, false);
});

test("latest decisive review state replaces an earlier approval", () => {
  const result = evaluateOwnership({
    rules: parseCodeowners(CODEOWNERS),
    files: [file("native_component_src/example_chip/driver.c")],
    reviews: [
      review("Sid9017", "APPROVED", HEAD, 1),
      review("Sid9017", "CHANGES_REQUESTED", HEAD, 2),
    ],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(result.success, false);
});

test("rename evaluates both previous and destination paths", () => {
  const files = [
    file("README.md", "renamed", "native_component_src/example_chip/old-driver.c"),
  ];
  assert.deepEqual(changedPaths(files), [
    "README.md",
    "native_component_src/example_chip/old-driver.c",
  ]);
  const result = evaluateOwnership({
    rules: parseCodeowners(CODEOWNERS),
    files,
    reviews: [],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(result.success, false);
  assert.match(result.blockers[0], /old-driver/);
});

test("unsupported patterns fail while parsing", () => {
  for (const policy of [
    "[ab].c @idy\n",
    "!docs/private @idy\n",
    "docs\\ private @idy\n",
  ]) {
    assert.throws(() => parseCodeowners(policy), PolicyError);
  }
});

test("effective ownerless rules block instead of falling back", () => {
  const result = evaluateOwnership({
    rules: parseCodeowners("* @idy\n/apps/github\n"),
    files: [file("apps/github/workflow.yml")],
    reviews: [],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(result.success, false);
  assert.match(result.blockers[0], /no direct user owner/);
});

test("effective team and email owners fail closed", () => {
  for (const owner of ["@h2vivi/firmware-team", "firmware@example.com"]) {
    const result = evaluateOwnership({
      rules: parseCodeowners(`* ${owner}\n`),
      files: [file("README.md")],
      reviews: [],
      author: "idy",
      headSha: HEAD,
    });
    assert.equal(result.success, false);
    assert.match(result.blockers[0], /unsupported owners/);
  }
});

test("missing ownership and malformed rename evidence fail closed", () => {
  const missing = evaluateOwnership({
    rules: parseCodeowners("/docs/ @idy\n"),
    files: [file("src/main.c")],
    reviews: [],
    author: "idy",
    headSha: HEAD,
  });
  assert.equal(missing.success, false);
  assert.match(missing.blockers[0], /no matching/);
  assert.throws(
    () => changedPaths([file("new.c", "renamed")]),
    PolicyError,
  );
});

test("status summary bounds and escapes untrusted path text", () => {
  const result = {
    success: false,
    paths: ["bad\npath"],
    approvalApprovers: [],
    selfOwned: [],
    approved: [],
    blockers: [`bad\npath: ${"x".repeat(70000)}`],
  };
  const summary = formatSummary(result, {author: "idy", headSha: HEAD});
  assert.equal(summary.includes("bad\npath"), false);
  assert.match(summary, /bad\\npath/);
  assert.ok(summary.length <= 60000);
});

test("runner publishes pending then successful Ownership eligibility statuses", async () => {
  const result = await runPublishedCheck({files: [file("README.md")]});
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  const statuses = result.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.deepEqual(
    statuses.map((request) => request.body.state),
    ["pending", "success"],
  );
  assert.ok(
    statuses.every(
      (request) => request.body.context === "Ownership eligibility",
    ),
  );
  assert.ok(
    statuses.every(
      (request) => request.body.context !== "OpenAI review eligibility",
    ),
  );
});

test("trusted ownership workflow can dispatch approved fork CI", async () => {
  const workflow = await readFile(OWNERSHIP_WORKFLOW, "utf8");
  assert.match(workflow, /permissions:\n  actions: write\n/);
  assert.match(workflow, /ref: \$\{\{ github\.event\.pull_request\.base\.sha/);
});

test("runner dispatches exact-head CI and OpenAI review after fork approval", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    reviews: [review("idy")],
    author: "contributor",
    eventKind: "workflow_run",
    headRepository: "contributor/gizos",
  });
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  const dispatches = result.requests.filter(
    (request) =>
      request.url ===
      "/repos/GizClaw/gizos/actions/workflows/ci.yml/dispatches",
  );
  assert.deepEqual(dispatches.map(({body}) => body), [
    {ref: "main", inputs: {checkout_sha: HEAD}},
  ]);
  const reviews = result.requests.filter(
    (request) =>
      request.url ===
      "/repos/GizClaw/gizos/actions/workflows/openai-pr-review.yml/dispatches",
  );
  assert.deepEqual(reviews.map(({body}) => body), [
    {
      ref: "main",
      inputs: {pull_request_number: "1", head_sha: HEAD},
    },
  ]);
  const statuses = result.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.equal(
    statuses.at(-1).body.description,
    "Ownership passed; approved fork checks were requested",
  );
});

test("runner does not dispatch fork CI without an independent approval", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    author: "idy",
    headRepository: "idy/gizos",
  });
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  assert.equal(
    result.requests.some((request) => request.url.endsWith("/dispatches")),
    false,
  );
});

test("runner does not redispatch approved fork CI for the same head", async () => {
  const statusHistory = [
    {
      context: "Ownership eligibility",
      state: "success",
      description: "Ownership passed; approved fork checks were requested",
      target_url: "https://github.example/actions/runs/previous",
    },
  ];
  const result = await runPublishedCheck({
    files: [file("README.md")],
    reviews: [review("idy")],
    author: "contributor",
    eventKind: "workflow_run",
    headRepository: "contributor/gizos",
    statusHistory,
  });
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  assert.equal(
    result.requests.some((request) => request.url.endsWith("/dispatches")),
    false,
  );
});

test("runner accepts a trusted workflow_run review request", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    eventKind: "workflow_run",
  });
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  assert.equal(
    result.requests.filter(
      (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
    ).length,
    2,
  );
});

test("runner keeps collaborator-pushed changes attributed to the PR author", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    author: "idy",
    eventActor: "collaborator",
  });
  assert.equal(result.exitCode, 0, result.stderr);
  assert.match(result.summary, /Author: @idy/);
  assert.doesNotMatch(result.summary, /collaborator/);
});

test("last-pushing path owner can approve the current head", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    reviews: [review("idy")],
    author: "contributor",
    eventActor: "idy",
  });
  assert.equal(result.exitCode, 0, result.stderr);
  const statuses = result.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.equal(statuses.at(-1).body.state, "success");
  assert.match(result.summary, /Author: @contributor/);
  assert.match(result.summary, /independent approval: 1/);
});

test("runner publishes policy failure without failing the publisher", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    author: "contributor",
  });
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  const statuses = result.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.equal(statuses.at(-1).body.state, "failure");
  assert.match(result.summary, /@idy/);
  assert.match(result.output, /eligible=false/);
});

test("same-head review reevaluation can replace policy failure with success", async () => {
  const statusHistory = [];
  const blocked = await runPublishedCheck({
    files: [file("README.md")],
    author: "contributor",
    runUrl: "https://github.example/actions/runs/1",
    statusHistory,
  });
  assert.equal(blocked.exitCode, 0, blocked.stderr || blocked.stdout);
  const blockedStatuses = blocked.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.equal(blockedStatuses.at(-1).body.state, "failure");

  const approved = await runPublishedCheck({
    files: [file("README.md")],
    reviews: [review("idy")],
    author: "contributor",
    eventKind: "workflow_run",
    runUrl: "https://github.example/actions/runs/2",
    statusHistory,
  });
  assert.equal(approved.exitCode, 0, approved.stderr || approved.stdout);
  const approvedStatuses = approved.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.equal(approvedStatuses.at(-1).body.state, "success");
  assert.match(approved.output, /eligible=true/);
  assert.equal(
    blockedStatuses.at(-1).url,
    approvedStatuses.at(-1).url,
    "both generations must publish to the same exact head",
  );
  assert.equal(
    approvedStatuses.at(-1).body.target_url,
    "https://github.example/actions/runs/2",
  );
  assert.deepEqual(
    statusHistory.map(({state, target_url: targetUrl}) => ({state, targetUrl})),
    [
      {state: "success", targetUrl: "https://github.example/actions/runs/2"},
      {state: "pending", targetUrl: "https://github.example/actions/runs/2"},
      {state: "failure", targetUrl: "https://github.example/actions/runs/1"},
      {state: "pending", targetUrl: "https://github.example/actions/runs/1"},
    ],
  );
});

test("runner replaces prior eligibility with pending before live API failure", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    pullRequestFailure: true,
  });
  assert.equal(result.exitCode, 1);
  const statuses = result.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.deepEqual(
    statuses.map((request) => request.body.state),
    ["pending", "failure"],
  );
  assert.match(result.summary, /fixture failure/);
});

test("runner cannot overwrite a newer same-head ownership generation", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    latestTargetUrl: "https://github.example/actions/runs/2",
  });
  assert.equal(result.exitCode, 1);
  const statuses = result.requests.filter(
    (request) => request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`,
  );
  assert.deepEqual(
    statuses.map((request) => request.body.state),
    ["pending"],
  );
  assert.match(result.stderr, /newer Ownership eligibility generation/);
});

test("status publication failure keeps the publisher job failed", async () => {
  const result = await runPublishedCheck({
    files: [file("README.md")],
    statusPublicationFailure: true,
  });
  assert.equal(result.exitCode, 1);
  assert.match(result.stderr, /status publication failure/);
});

function publishedStates(result) {
  return result.requests
    .filter((request) => request.method === "POST" && /\/statuses\//.test(request.url))
    .map((request) => request.body.state);
}

async function largeGitFixture(t) {
  const root = await mkdtemp(`${tmpdir()}/ownership-fixture-`);
  t.after(() => rm(root, {recursive: true, force: true}));
  const repositoryRoot = resolve(root, "trusted");
  await mkdir(resolve(repositoryRoot, ".github"), {recursive: true});
  const policy = "* @idy\n/protected/ @security\n";
  await writeFile(resolve(repositoryRoot, ".github/CODEOWNERS"), policy);
  const env = {
    ...Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith("GIT_"))),
    GIT_CONFIG_NOSYSTEM: "1",
    GIT_CONFIG_GLOBAL: "/dev/null",
    GIT_AUTHOR_NAME: "Ownership Fixture",
    GIT_AUTHOR_EMAIL: "fixture@example.invalid",
    GIT_COMMITTER_NAME: "Ownership Fixture",
    GIT_COMMITTER_EMAIL: "fixture@example.invalid",
  };
  const git = (args, input) => execFileSync("git", args, {
    cwd: repositoryRoot, env, input, encoding: "utf8", stdio: ["pipe", "pipe", "pipe"],
  }).trim();
  git(["init", "--template=", "--initial-branch=main"]);
  const blob = (text) => git(["hash-object", "-w", "--stdin"], text);
  const tree = (entries) => {
    git(["read-tree", "--empty"]);
    git(["update-index", "-z", "--index-info"], [...entries].map(([path, [mode, sha]]) => `${mode} ${sha}\t${path}\0`).join(""));
    return git(["write-tree"]);
  };
  const original = new Map([
    [".github/CODEOWNERS", ["100644", blob(policy)]],
    ["protected/old-driver.c", ["100644", blob(Array.from({length: 100}, (_, index) => `original driver line ${index}\n`).join(""))]],
  ]);
  const mergeBaseSha = git(["commit-tree", tree(original), "-m", "common ancestor"]);
  const addedPaths = [
    ...Array.from({length: 3240}, (_, index) => `added/file-${index}.c`),
    "--output=ownership-executed",
    "space dir/file name.c",
    "tab\tname.c",
    "protected/line\nname.c",
    "quote\"'\\name.c",
    "$(touch ownership-executed).c",
    "`touch ownership-executed`.c",
    "unicodé/文档.c",
  ];
  const head = new Map(original);
  head.set("moved-driver.c", ["100644", blob(Array.from({length: 100}, (_, index) => index === 50 ? "modified driver line\n" : `original driver line ${index}\n`).join(""))]);
  head.delete("protected/old-driver.c");
  const addedBlob = blob("added file\n");
  for (const path of addedPaths) head.set(path, ["100644", addedBlob]);
  const extras = new Map([
    [".gitattributes", ["100644", blob("* diff=untrusted\n")]],
    [".gitmodules", ["100644", blob('[submodule "module"]\npath = nested/module\nurl = /untrusted\n')]],
    [".hooks/post-checkout", ["100755", blob("#!/bin/sh\ntouch ownership-executed\n")]],
    ["link-to-parent", ["120000", blob("../ownership-executed")]],
    ["nested/module", ["160000", mergeBaseSha]],
  ]);
  for (const [path, entry] of extras) head.set(path, entry);
  const headSha = git(["commit-tree", tree(head), "-p", mergeBaseSha, "-m", "large untrusted head"]);
  git(["update-ref", "refs/heads/topic", headSha]);
  const base = new Map(original);
  base.set("protected/base-only.c", ["100644", blob("advanced base only\n")]);
  const baseSha = git(["commit-tree", tree(base), "-p", mergeBaseSha, "-m", "base advanced independently"]);
  git(["update-ref", "refs/heads/main", baseSha]);
  return {
    root, git, repositoryRoot, headSha, baseSha, mergeBaseSha,
    changedFilesCount: addedPaths.length + extras.size + 1,
    options: {
      repositoryRoot, headSha, gitRemote: repositoryRoot,
      changedFilesCount: addedPaths.length + extras.size + 1,
    },
  };
}

function assertEvidenceFailed(result) {
  assert.equal(result.exitCode, 1, result.stderr || result.stdout);
  assert.deepEqual(publishedStates(result), ["pending", "failure"]);
  assert.match(result.output, /eligible=false/);
  assert.equal(result.requests.some((request) => request.url.endsWith("/dispatches")), false);
}

test("runner uses complete Git evidence above 3000 files and checks both rename owners", async (t) => {
  const fixture = await largeGitFixture(t);
  const blocked = await runPublishedCheck(fixture.options);
  assert.equal(blocked.exitCode, 0, blocked.stderr || blocked.summary);
  assert.deepEqual(publishedStates(blocked), ["pending", "failure"]);
  assert.match(blocked.summary, /protected\/old-driver.c: requires current-head approval from @security/);
  assert.doesNotMatch(blocked.summary, /base-only/);
  assert.match(blocked.summary, new RegExp(`Evaluated paths: ${fixture.changedFilesCount + 1}`));
  assert.equal(blocked.requests.some((request) => request.url.includes("/files?")), false);
  const approved = await runPublishedCheck({
    ...fixture.options,
    reviews: [review("security", "APPROVED", fixture.headSha)],
  });
  assert.equal(approved.exitCode, 0, approved.stderr || approved.summary);
  assert.deepEqual(publishedStates(approved), ["pending", "success"]);
  assert.match(approved.summary, /Paths satisfied by independent approval: 2/);
  const fetch = approved.gitCommands.find(({args}) => args[0] === "fetch");
  assert.deepEqual(fetch.args.slice(-4), ["--", "https://github.com/GizClaw/gizos.git", fixture.baseSha, fixture.headSha]);
  const diff = approved.gitCommands.find(({args}) => args[0] === "diff");
  assert.deepEqual(diff.args.slice(-3), [fixture.mergeBaseSha, fixture.headSha, "--"]);
  assert.ok(diff.args.includes("-z") && diff.args.includes("--no-ext-diff") && diff.args.includes("--no-textconv"));
  assert.equal(approved.gitCommands.some(({args}) => ["checkout", "switch", "submodule"].includes(args[0])), false);
  await assert.rejects(readFile(resolve(fetch.cwd, "HEAD")), {code: "ENOENT"});
  await assert.rejects(readFile(resolve(fixture.repositoryRoot, "ownership-executed")), {code: "ENOENT"});
  await assert.rejects(readFile(resolve(fixture.repositoryRoot, "moved-driver.c")), {code: "ENOENT"});
});

test("runner uses the API through its exact 3000-file boundary", async () => {
  const result = await runPublishedCheck({
    files: Array.from({length: 3000}, (_, index) => file(`file-${index}.c`)),
  });
  assert.equal(result.exitCode, 0, result.stderr || result.summary);
  assert.deepEqual(publishedStates(result), ["pending", "success"]);
  assert.equal(result.requests.filter((request) => request.url.includes("/files?")).length, 31);
});

test("runner rejects truncated API evidence and duplicate file records", async () => {
  for (const options of [
    {files: [file("one.c")], changedFilesCount: 2},
    {files: [file("one.c"), file("one.c")]},
  ]) {
    const result = await runPublishedCheck(options);
    assertEvidenceFailed(result);
    assert.match(result.summary, /changed-file evidence is incomplete/);
  }
});

test("Git failures and count mismatches cannot fall back to capped evidence", async (t) => {
  const fixture = await largeGitFixture(t);
  for (const override of [
    {gitFailure: "fetch"},
    {gitFailure: "diff"},
    {changedFilesCount: fixture.changedFilesCount + 1},
  ]) {
    const result = await runPublishedCheck({...fixture.options, ...override});
    assertEvidenceFailed(result);
    assert.equal(result.requests.some((request) => request.url.includes("/files?")), false);
    const init = result.gitCommands.find(({args}) => args[0] === "init");
    await assert.rejects(readFile(resolve(init.cwd, "HEAD")), {code: "ENOENT"});
  }
});

test("Git evidence ignores inherited configuration and does not execute path text", async (t) => {
  const fixture = await largeGitFixture(t);
  const marker = resolve(fixture.root, "executed");
  const hostileCommand = resolve(fixture.root, "hostile-command");
  await writeFile(hostileCommand, `#!/bin/sh\ntouch '${marker}'\n`, {mode: 0o755});
  const result = await runPublishedCheck({
    ...fixture.options,
    reviews: [review("security", "APPROVED", fixture.headSha)],
    gitEnvironment: {
      GIT_CONFIG_COUNT: "3",
      GIT_CONFIG_KEY_0: "diff.external",
      GIT_CONFIG_VALUE_0: hostileCommand,
      GIT_CONFIG_KEY_1: "diff.untrusted.textconv",
      GIT_CONFIG_VALUE_1: hostileCommand,
      GIT_CONFIG_KEY_2: "core.sshCommand",
      GIT_CONFIG_VALUE_2: hostileCommand,
      GIT_EXTERNAL_DIFF: hostileCommand,
      GIT_DIR: "/nonexistent/untrusted",
      GIT_WORK_TREE: "/nonexistent/untrusted",
      GIT_ALTERNATE_OBJECT_DIRECTORIES: "/nonexistent/untrusted",
    },
  });
  assert.equal(result.exitCode, 0, result.stderr || result.summary);
  assert.deepEqual(publishedStates(result), ["pending", "success"]);
  assert.ok(result.gitCommands.some(({args}) => args[0] === "diff"));
  await assert.rejects(readFile(marker), {code: "ENOENT"});
});

test("Git evidence refuses ambiguous merge bases", async (t) => {
  const fixture = await largeGitFixture(t);
  const left = fixture.git(["commit-tree", `${fixture.baseSha}^{tree}`, "-p", fixture.baseSha, "-p", fixture.headSha, "-m", "left merge"]);
  const right = fixture.git(["commit-tree", `${fixture.headSha}^{tree}`, "-p", fixture.headSha, "-p", fixture.baseSha, "-m", "right merge"]);
  fixture.git(["update-ref", "refs/heads/main", left]);
  fixture.git(["update-ref", "refs/heads/topic", right]);
  const result = await runPublishedCheck({...fixture.options, headSha: right});
  assertEvidenceFailed(result);
  assert.match(result.summary, /exactly one merge base/);
});

test("large fork evidence fetches only from the trusted base repository", async (t) => {
  const fixture = await largeGitFixture(t);
  const result = await runPublishedCheck({
    ...fixture.options,
    headRepository: "untrusted/gizos",
    eventKind: "workflow_run",
    reviews: [review("security", "APPROVED", fixture.headSha)],
  });
  assert.equal(result.exitCode, 0, result.stderr || result.summary);
  const fetch = result.gitCommands.find(({args}) => args[0] === "fetch");
  assert.equal(fetch.args.at(-3), "https://github.com/GizClaw/gizos.git");
  const dispatch = result.requests.find((request) => request.url.endsWith("/ci.yml/dispatches"));
  assert.deepEqual(dispatch.body, {ref: "main", inputs: {checkout_sha: fixture.headSha}});
});

test("Git evidence rejects unsafe identities before invoking Git", async () => {
  const options = {
    repository: "GizClaw/gizos", serverUrl: "https://github.com",
    baseSha: HEAD, headSha: HEAD, expectedCount: 3001,
  };
  for (const update of [
    {baseSha: "--upload-pack=untrusted"}, {headSha: `${HEAD};touch unsafe`},
    {repository: "owner/repo;touch unsafe"}, {repository: "../other/repo"},
    {serverUrl: "file:///untrusted"}, {serverUrl: "ssh://github.com"},
    {serverUrl: "https://github.com/other/"}, {serverUrl: "https://user:password@github.com"},
    {serverUrl: "https://github.com?redirect=untrusted"},
  ]) {
    await assert.rejects(gitChangedFiles({...options, ...update}));
  }
});

test("dismissed approval during Git collection cannot authorize fork execution", async (t) => {
  const fixture = await largeGitFixture(t);
  const result = await runPublishedCheck({
    ...fixture.options,
    headRepository: "untrusted/gizos",
    eventKind: "workflow_run",
    reviews: [review("security", "APPROVED", fixture.headSha)],
    reviewsAfterGit: [review("security", "DISMISSED", fixture.headSha)],
  });
  assert.equal(result.exitCode, 0, result.stderr || result.summary);
  assert.deepEqual(publishedStates(result), ["pending", "failure"]);
  assert.match(result.summary, /requires current-head approval from @security/);
  assert.equal(result.requests.some((request) => request.url.endsWith("/dispatches")), false);
  const reviewRequests = result.requests.filter((request) => request.url.includes("/reviews?"));
  assert.ok(reviewRequests.length > 0);
  assert.ok(reviewRequests.every((request) => request.gitEvidenceComplete));
});

test("line separators cannot bypass directory or globstar ownership", () => {
  for (const separator of ["\n", "\r", String.fromCodePoint(0x2028), String.fromCodePoint(0x2029)]) {
    for (const [pattern, path] of [
      ["/protected/", `protected/new${separator}name.c`],
      ["/protected", `protected/nested${separator}dir/name.c`],
      ["**/protected/*.c", `nested${separator}dir/protected/name.c`],
    ]) {
      const input = {
        rules: parseCodeowners(`* @idy\n${pattern} @security\n`),
        files: [file(path, "added")], author: "idy", headSha: HEAD,
      };
      const blocked = evaluateOwnership({...input, reviews: []});
      assert.equal(blocked.success, false, JSON.stringify({pattern, path}));
      assert.match(blocked.blockers[0], /@security/);
      assert.equal(evaluateOwnership({...input, reviews: [review("security")]}).success, true);
    }
  }
});

test("literal ownership patterns require the absolute end of a path", () => {
  const rules = parseCodeowners("* @security\n/owned.c @idy\n");
  for (const separator of ["\n", "\r", String.fromCodePoint(0x2028), String.fromCodePoint(0x2029)]) {
    const result = evaluateOwnership({
      rules, files: [file(`owned.c${separator}`)], reviews: [], author: "idy", headSha: HEAD,
    });
    assert.equal(result.success, false);
    assert.match(result.blockers[0], /@security/);
  }
});

test("Git parser preserves special and rename paths without quoting or splitting", () => {
  const source = "protected/old\nname.c";
  const destination = "--new\tname \"'\\$(touch x)文.c";
  const files = parseGitFiles(Buffer.from(`R100\0${source}\0${destination}\0T\0symlink\0`), 2);
  assert.deepEqual(files, [file(destination, "renamed", source), {filename: "symlink", status: "changed"}]);
  assert.deepEqual(changedPaths(files), [destination, source, "symlink"]);
});

test("Git parser fails closed on truncated, invalid, duplicate or unsupported metadata", () => {
  for (const output of [
    "M\0one.c", "R100\0old.c\0", "A\0../outside\0", "A\0/absolute\0",
    "A\0empty//segment\0", "A\0one.c\0M\0one.c\0", "C100\0old.c\0new.c\0",
    "U\0unmerged.c\0", "R101\0old.c\0new.c\0", "M\0\0",
  ]) {
    assert.throws(() => parseGitFiles(Buffer.from(output), 1), PolicyError);
  }
  assert.throws(() => parseGitFiles(Buffer.from([77, 0, 255, 0]), 1), /UTF-8/);
  assert.throws(() => parseGitFiles(Buffer.from("A\0one.c\0"), 2), /incomplete/);
  for (const count of [0, -1, undefined, "1", 1.5, Number.NaN]) {
    assert.throws(() => assertCompleteFiles([file("one.c")], count), PolicyError);
  }
});

test("runner rejects stale event and changed live PR identities before publication", async () => {
  const changedHead = {sha: "f".repeat(40), repo: {full_name: "GizClaw/gizos"}};
  const updates = [
    {head: changedHead},
    {base: {sha: "f".repeat(40), ref: "main", repo: {full_name: "GizClaw/gizos"}}},
    {base: {sha: "f".repeat(40), ref: "release", repo: {full_name: "other/gizos"}}},
    {user: {login: "someone-else"}},
    {head: {sha: HEAD, repo: {full_name: "other/gizos"}}},
    {state: "closed"},
    {changed_files: 2},
  ];
  for (const update of updates) {
    for (const pullRequestUpdates of [[update], [{}, update]]) {
      const result = await runPublishedCheck({files: [file("README.md")], pullRequestUpdates});
      assertEvidenceFailed(result);
      assert.match(result.summary, /identity is stale or incomplete/);
    }
  }
  const stale = await runPublishedCheck({
    files: [file("README.md")], eventKind: "workflow_run", eventUpdate: {head: changedHead},
  });
  assert.equal(stale.exitCode, 1);
  assert.match(stale.summary, /identity is stale or incomplete/);
});

test("runner rejects a head change while collecting large Git evidence", async (t) => {
  const fixture = await largeGitFixture(t);
  const result = await runPublishedCheck({
    ...fixture.options,
    reviews: [review("security", "APPROVED", fixture.headSha)],
    pullRequestUpdates: [{}, {head: {sha: "f".repeat(40), repo: {full_name: "GizClaw/gizos"}}}],
  });
  assertEvidenceFailed(result);
  assert.ok(result.gitCommands.some(({args}) => args[0] === "diff"));
  assert.match(result.summary, /identity is stale or incomplete/);
});
