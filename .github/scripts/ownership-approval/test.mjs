import assert from "node:assert/strict";
import {spawn, execFileSync} from "node:child_process";
import {mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
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

const HEAD = "0123456789abcdef0123456789abcdef01234567";
const SCRIPT_DIRECTORY = dirname(fileURLToPath(import.meta.url));
const REPOSITORY_ROOT = resolve(SCRIPT_DIRECTORY, "../../..");
const RUN_SCRIPT = resolve(SCRIPT_DIRECTORY, "run.mjs");
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
  files,
  reviews = [],
  author = "idy",
  authorPermission = "read",
  eventActor = author,
  eventKind = "pull_request_target",
  pullRequestFailure = false,
  runUrl = "https://github.example/actions/runs/1",
  latestTargetUrl = runUrl,
  statusHistory = null,
  statusPublicationFailure = false,
}) {
  const baseSha = execFileSync("git", ["rev-parse", "HEAD"], {
    cwd: REPOSITORY_ROOT,
    encoding: "utf8",
  }).trim();
  const requests = [];
  const server = createServer(async (request, response) => {
    const chunks = [];
    for await (const chunk of request) {
      chunks.push(chunk);
    }
    const bodyText = Buffer.concat(chunks).toString("utf8");
    const body = bodyText ? JSON.parse(bodyText) : null;
    requests.push({method: request.method, url: request.url, body});

    let payload;
    if (request.url === "/repos/GizClaw/gizos/pulls/1") {
      if (pullRequestFailure) {
        response.writeHead(500, {"Content-Type": "application/json"});
        response.end(JSON.stringify({message: "fixture failure"}));
        return;
      }
      payload = {
        number: 1,
        state: "open",
        changed_files: files.length,
        user: {login: author},
        head: {sha: HEAD},
        base: {sha: baseSha},
      };
    } else if (
      request.method === "GET" &&
      request.url ===
        `/repos/GizClaw/gizos/collaborators/${encodeURIComponent(author)}/permission`
    ) {
      payload = {permission: authorPermission};
    } else if (
      request.method === "GET" &&
      request.url ===
        `/repos/GizClaw/gizos/commits/${HEAD}/statuses?per_page=100`
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
      request.url === `/repos/GizClaw/gizos/statuses/${HEAD}`
    ) {
      if (statusPublicationFailure) {
        response.writeHead(500, {"Content-Type": "application/json"});
        response.end(JSON.stringify({message: "status publication failure"}));
        return;
      }
      statusHistory?.unshift(body);
      payload = {id: requests.length};
    } else if (request.url.startsWith("/repos/GizClaw/gizos/pulls/1/files?")) {
      payload = files;
    } else if (request.url.startsWith("/repos/GizClaw/gizos/pulls/1/reviews?")) {
      payload = reviews;
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
  const pullRequest = {
    number: 1,
    user: {login: author},
    head: {sha: HEAD},
    base: {sha: baseSha},
  };
  const event =
    eventKind === "workflow_run"
      ? {
          workflow_run: {
            name: "Ownership Approval Review Request",
            event: "pull_request_review",
            conclusion: "success",
            pull_requests: [pullRequest],
          },
        }
      : {
          action: "synchronize",
          sender: {login: eventActor},
          pull_request: {
            ...pullRequest,
          },
        };
  await writeFile(eventPath, JSON.stringify(event));

  try {
    const child = spawn(process.execPath, [RUN_SCRIPT], {
      cwd: REPOSITORY_ROOT,
      env: {
        ...process.env,
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
    return {exitCode, requests, stdout, stderr, summary, output};
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
