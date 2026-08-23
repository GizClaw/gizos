import assert from "node:assert/strict";
import {execFileSync, spawn} from "node:child_process";
import {createServer} from "node:http";
import {mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, resolve} from "node:path";
import test from "node:test";
import {fileURLToPath} from "node:url";

import {
  EligibilityStatusError,
  OPENAI_ELIGIBILITY_CONTEXT,
  OWNERSHIP_ELIGIBILITY_CONTEXT,
  openAiFinalStatus,
  statusPayload,
} from "./common.mjs";

const HEAD = "0123456789abcdef0123456789abcdef01234567";
const STALE_HEAD = "fedcba9876543210fedcba9876543210fedcba98";
const SCRIPT_DIRECTORY = dirname(fileURLToPath(import.meta.url));
const REPOSITORY_ROOT = resolve(SCRIPT_DIRECTORY, "../../..");
const RUN_SCRIPT = resolve(SCRIPT_DIRECTORY, "run.mjs");
const OPENAI_WORKFLOW = resolve(
  REPOSITORY_ROOT,
  ".github/workflows/openai-pr-review.yml",
);
const OWNERSHIP_WORKFLOW = resolve(
  REPOSITORY_ROOT,
  ".github/workflows/ownership-approval.yml",
);
const BASE = execFileSync("git", ["rev-parse", "HEAD"], {
  cwd: REPOSITORY_ROOT,
  encoding: "utf8",
}).trim();
const PASS_EVIDENCE_OBJECT = {
  schema_version: 2,
  repository: "GizClaw/gizos",
  pull_request_number: 1,
  base_sha: BASE,
  head_sha: HEAD,
  snapshot_sha256: "a".repeat(64),
  trusted_policy_sha256: "b".repeat(64),
  workflow_source_sha: "c".repeat(40),
  verdict: "pass",
  stage_verdicts: {
    pr_review: "pass",
    issue_review: "pass",
    code_review: "pass",
  },
  blockers: [],
};
const PASS_EVIDENCE = JSON.stringify(PASS_EVIDENCE_OBJECT);
const POLICY_FAILURE_CASES = [
  {
    stage: "pr_review",
    blocker: {
      source: "pr-format",
      code: "invalid-title",
      message: "Pull-request title does not match the configured format.",
    },
  },
  {
    stage: "issue_review",
    blocker: {
      source: "issue-design",
      code: "incomplete-plan",
      message: "The linked Issue plan is incomplete.",
    },
  },
  {
    stage: "code_review",
    blocker: {
      source: "code-review",
      code: "P1",
      message: "src/example.mjs:10: The implementation is incorrect.",
    },
  },
];

function policyFailureEvidence({stage, blocker}) {
  return JSON.stringify({
    ...PASS_EVIDENCE_OBJECT,
    verdict: "fail",
    stage_verdicts: {
      ...PASS_EVIDENCE_OBJECT.stage_verdicts,
      [stage]: "fail",
    },
    blockers: [blocker],
  });
}

function finalStatus(readinessEvidence, reviewResult = "success") {
  return openAiFinalStatus({
    reviewResult,
    readinessEvidence,
    expectedRepository: "GizClaw/gizos",
    expectedPullRequestNumber: 1,
    expectedBaseSha: BASE,
    expectedHeadSha: HEAD,
    expectedWorkflowSourceSha: "c".repeat(40),
  });
}

async function runPublisher({
  mode,
  currentHead = HEAD,
  reviewResult = "success",
  readinessEvidence = PASS_EVIDENCE,
  apiFailure = false,
  statusPostFailure = false,
  runUrl = "https://github.example/actions/runs/1",
  latestTargetUrl = runUrl,
}) {
  const requests = [];
  const server = createServer(async (request, response) => {
    const chunks = [];
    for await (const chunk of request) {
      chunks.push(chunk);
    }
    const bodyText = Buffer.concat(chunks).toString("utf8");
    const body = bodyText ? JSON.parse(bodyText) : null;
    requests.push({method: request.method, url: request.url, body});
    if (apiFailure) {
      response.writeHead(500, {"Content-Type": "application/json"});
      response.end(JSON.stringify({message: "fixture failure"}));
      return;
    }
    if (request.url === "/repos/GizClaw/gizos/pulls/1") {
      response.writeHead(200, {"Content-Type": "application/json"});
      response.end(
        JSON.stringify({
          number: 1,
          state: "open",
          draft: false,
          head: {sha: currentHead},
          base: {sha: BASE, ref: "main"},
        }),
      );
      return;
    }
    if (
      request.method === "GET" &&
      request.url ===
        `/repos/GizClaw/gizos/commits/${HEAD}/statuses?per_page=100`
    ) {
      response.writeHead(200, {"Content-Type": "application/json"});
      response.end(
        JSON.stringify([
          {
            context: OPENAI_ELIGIBILITY_CONTEXT,
            state: "pending",
            target_url: latestTargetUrl,
          },
        ]),
      );
      return;
    }
    if (
      request.method === "POST" &&
      request.url.startsWith("/repos/GizClaw/gizos/statuses/")
    ) {
      if (statusPostFailure) {
        response.writeHead(500, {"Content-Type": "application/json"});
        response.end(JSON.stringify({message: "fixture publication failure"}));
        return;
      }
      response.writeHead(201, {"Content-Type": "application/json"});
      response.end(JSON.stringify({id: requests.length}));
      return;
    }
    response.writeHead(404, {"Content-Type": "application/json"});
    response.end(
      JSON.stringify({message: `unhandled ${request.method} ${request.url}`}),
    );
  });
  await new Promise((resolveListen) =>
    server.listen(0, "127.0.0.1", resolveListen),
  );
  const address = server.address();
  const temporaryDirectory = await mkdtemp(`${tmpdir()}/eligibility-status-`);
  const eventPath = resolve(temporaryDirectory, "event.json");
  const outputPath = resolve(temporaryDirectory, "output.txt");
  await writeFile(
    eventPath,
    JSON.stringify({
      issue: {
        number: 1,
        pull_request: {
          url: "https://api.github.example/repos/GizClaw/gizos/pulls/1",
        },
      },
    }),
  );
  await writeFile(outputPath, "");

  try {
    const child = spawn(process.execPath, [RUN_SCRIPT], {
      cwd: REPOSITORY_ROOT,
      env: {
        ...process.env,
        ELIGIBILITY_STATUS_MODE: mode,
        EXPECTED_BASE_SHA: BASE,
        EXPECTED_HEAD_SHA: HEAD,
        EXPECTED_REVIEW_WORKFLOW_SHA: "c".repeat(40),
        GITHUB_API_URL: `http://127.0.0.1:${address.port}`,
        GITHUB_EVENT_PATH: eventPath,
        GITHUB_OUTPUT: outputPath,
        GITHUB_REPOSITORY: "GizClaw/gizos",
        GITHUB_RUN_URL: runUrl,
        GITHUB_TOKEN: "test-token",
        PULL_REQUEST_NUMBER: "1",
        READINESS_EVIDENCE: readinessEvidence,
        REVIEW_RESULT: reviewResult,
        TRUSTED_SOURCE_SHA: BASE,
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
    const outputs = await readFile(outputPath, "utf8");
    return {exitCode, outputs, requests, stderr, stdout};
  } finally {
    await new Promise((resolveClose) => server.close(resolveClose));
    await rm(temporaryDirectory, {recursive: true, force: true});
  }
}

test("status payload accepts only the two isolated eligibility contexts", () => {
  for (const context of [
    OPENAI_ELIGIBILITY_CONTEXT,
    OWNERSHIP_ELIGIBILITY_CONTEXT,
  ]) {
    assert.equal(
      statusPayload({
        state: "success",
        context,
        description: "policy passed",
        targetUrl: "https://github.example/actions/runs/1",
      }).context,
      context,
    );
  }
  assert.throws(
    () =>
      statusPayload({
        state: "success",
        context: "OpenAI PR Review",
        description: "policy passed",
        targetUrl: "https://github.example/actions/runs/1",
      }),
    EligibilityStatusError,
  );
});

test("policy workflows serialize complete status generations per pull request", async () => {
  for (const workflowPath of [OPENAI_WORKFLOW, OWNERSHIP_WORKFLOW]) {
    const workflow = await readFile(workflowPath, "utf8");
    assert.match(workflow, /concurrency:\n  group: [^\n]+\n  cancel-in-progress: false/);
  }
});

test("OpenAI review runs only for an explicit PR comment request", async () => {
  const workflow = await readFile(OPENAI_WORKFLOW, "utf8");
  assert.doesNotMatch(workflow, /pull_request_target:/);
  assert.match(workflow, /issue_comment:\n    types: \[created\]/);
  assert.match(
    workflow,
    /github\.event\.issue\.pull_request &&\n      startsWith\(github\.event\.comment\.body, '@codex'\)/,
  );
});

test("status payload rejects unbounded descriptions and unsafe target URLs", () => {
  assert.throws(
    () =>
      statusPayload({
        state: "success",
        context: OPENAI_ELIGIBILITY_CONTEXT,
        description: "x".repeat(141),
        targetUrl: "https://github.example/actions/runs/1",
      }),
    EligibilityStatusError,
  );
  assert.throws(
    () =>
      statusPayload({
        state: "success",
        context: OPENAI_ELIGIBILITY_CONTEXT,
        description: "policy passed",
        targetUrl: "http://github.example/actions/runs/1",
      }),
    EligibilityStatusError,
  );
});

test("OpenAI aggregation distinguishes policy verdicts from invalid evidence", () => {
  assert.deepEqual(finalStatus(PASS_EVIDENCE), {
    state: "success",
    description: "OpenAI review policy passed for this exact head",
    publisherSucceeded: true,
  });
  for (const policyFailure of POLICY_FAILURE_CASES) {
    assert.deepEqual(finalStatus(policyFailureEvidence(policyFailure)), {
      state: "failure",
      description: "OpenAI review policy is not ready for this exact head",
      publisherSucceeded: true,
    });
  }

  for (const evidence of [
    "not-json",
    JSON.stringify({...PASS_EVIDENCE_OBJECT, head_sha: STALE_HEAD}),
    JSON.stringify({
      ...PASS_EVIDENCE_OBJECT,
      workflow_source_sha: "d".repeat(40),
    }),
    JSON.stringify({
      ...PASS_EVIDENCE_OBJECT,
      stage_verdicts: {
        pr_review: "pass",
        issue_review: "pass",
        code_review: "fail",
      },
      blockers: [],
    }),
    JSON.stringify({
      ...PASS_EVIDENCE_OBJECT,
      stage_verdicts: {
        pr_review: "pass",
        issue_review: "pass",
        code_review: "pass",
      },
      blockers: [{
        source: "code-review",
        code: "P1",
        message: "Unexpected blocker for a passing stage.",
      }],
    }),
    JSON.stringify({
      ...PASS_EVIDENCE_OBJECT,
      verdict: "fail",
      stage_verdicts: {
        ...PASS_EVIDENCE_OBJECT.stage_verdicts,
        code_review: "fail",
      },
      blockers: [{
        source: "unknown-source",
        code: "unknown",
        message: "Unknown blocker source.",
      }],
    }),
    JSON.stringify({
      ...PASS_EVIDENCE_OBJECT,
      verdict: "fail",
      stage_verdicts: {
        ...PASS_EVIDENCE_OBJECT.stage_verdicts,
        code_review: "fail",
      },
      blockers: [{source: "code-review", code: "P1"}],
    }),
  ]) {
    const result = finalStatus(evidence);
    assert.equal(result.state, "failure");
    assert.equal(result.publisherSucceeded, false);
  }
});

test("start publishes pending on the live exact head and exports its identity", async () => {
  const result = await runPublisher({mode: "start"});
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  const status = result.requests.find((request) =>
    request.url.endsWith(`/statuses/${HEAD}`),
  );
  assert.equal(status.body.context, OPENAI_ELIGIBILITY_CONTEXT);
  assert.equal(status.body.state, "pending");
  assert.match(result.outputs, /pull_request_number=1/);
  assert.match(result.outputs, new RegExp(`head_sha=${HEAD}`));
  assert.match(result.outputs, new RegExp(`base_sha=${BASE}`));
});

test("finish publishes success only for matching live identity and complete evidence", async () => {
  const result = await runPublisher({mode: "finish"});
  assert.equal(result.exitCode, 0, result.stderr || result.stdout);
  const status = result.requests.find((request) =>
    request.url.endsWith(`/statuses/${HEAD}`),
  );
  assert.equal(status.body.context, OPENAI_ELIGIBILITY_CONTEXT);
  assert.equal(status.body.state, "success");
  assert.notEqual(status.body.context, OWNERSHIP_ELIGIBILITY_CONTEXT);
});

test("valid policy failures publish failure without failing the publisher", async () => {
  for (const policyFailure of POLICY_FAILURE_CASES) {
    const result = await runPublisher({
      mode: "finish",
      readinessEvidence: policyFailureEvidence(policyFailure),
    });
    assert.equal(result.exitCode, 0, result.stderr || result.stdout);
    const status = result.requests.find((request) =>
      request.url.endsWith(`/statuses/${HEAD}`),
    );
    assert.equal(status.body.context, OPENAI_ELIGIBILITY_CONTEXT);
    assert.equal(status.body.state, "failure");
    assert.notEqual(status.body.context, OWNERSHIP_ELIGIBILITY_CONTEXT);
  }
});

test("a same-head re-review can replace a valid policy failure with success", async () => {
  const failed = await runPublisher({
    mode: "finish",
    readinessEvidence: policyFailureEvidence(POLICY_FAILURE_CASES[1]),
    runUrl: "https://github.example/actions/runs/1",
  });
  const recovered = await runPublisher({
    mode: "finish",
    runUrl: "https://github.example/actions/runs/2",
  });
  assert.equal(failed.exitCode, 0, failed.stderr || failed.stdout);
  assert.equal(recovered.exitCode, 0, recovered.stderr || recovered.stdout);
  const failedStatus = failed.requests.find((request) =>
    request.url.endsWith(`/statuses/${HEAD}`),
  );
  const recoveredStatus = recovered.requests.find((request) =>
    request.url.endsWith(`/statuses/${HEAD}`),
  );
  assert.equal(failedStatus.body.context, OPENAI_ELIGIBILITY_CONTEXT);
  assert.equal(recoveredStatus.body.context, OPENAI_ELIGIBILITY_CONTEXT);
  assert.equal(failedStatus.body.state, "failure");
  assert.equal(recoveredStatus.body.state, "success");
  assert.equal(
    failedStatus.url,
    recoveredStatus.url,
    "both generations must publish to the same exact head",
  );
  assert.equal(
    recoveredStatus.body.target_url,
    "https://github.example/actions/runs/2",
  );
});

test("finish rejects a stale head without touching the current head", async () => {
  const result = await runPublisher({mode: "finish", currentHead: STALE_HEAD});
  assert.equal(result.exitCode, 1);
  const statusRequests = result.requests.filter((request) =>
    request.url.includes("/statuses/"),
  );
  assert.equal(statusRequests.length, 1);
  assert.ok(statusRequests[0].url.endsWith(`/statuses/${HEAD}`));
  assert.equal(statusRequests[0].body.state, "failure");
  assert.ok(
    statusRequests.every(
      (request) => !request.url.endsWith(`/statuses/${STALE_HEAD}`),
    ),
  );
});

test("a superseded generation cannot overwrite the newer same-head status", async () => {
  const result = await runPublisher({
    mode: "finish",
    latestTargetUrl: "https://github.example/actions/runs/2",
  });
  assert.equal(result.exitCode, 1);
  assert.match(result.stderr, /newer OpenAI review eligibility generation/);
  assert.equal(
    result.requests.filter((request) => request.method === "POST").length,
    0,
  );
});

test("review failure and invalid evidence publish failure", async () => {
  for (const inputs of [
    {reviewResult: "failure", readinessEvidence: PASS_EVIDENCE},
    {reviewResult: "cancelled", readinessEvidence: PASS_EVIDENCE},
    {reviewResult: "success", readinessEvidence: "not-json"},
    {
      reviewResult: "success",
      readinessEvidence: JSON.stringify({
        ...PASS_EVIDENCE_OBJECT,
        head_sha: STALE_HEAD,
      }),
    },
    {
      reviewResult: "success",
      readinessEvidence: JSON.stringify({
        ...PASS_EVIDENCE_OBJECT,
        verdict: "fail",
      }),
    },
    {
      reviewResult: "success",
      readinessEvidence: JSON.stringify({
        ...PASS_EVIDENCE_OBJECT,
        verdict: "fail",
        stage_verdicts: {
          ...PASS_EVIDENCE_OBJECT.stage_verdicts,
          code_review: "fail",
        },
        blockers: [{
          source: "unknown-source",
          code: "unknown",
          message: "Unknown blocker source.",
        }],
      }),
    },
  ]) {
    const result = await runPublisher({mode: "finish", ...inputs});
    assert.equal(result.exitCode, 1);
    const status = result.requests.find((request) =>
      request.url.endsWith(`/statuses/${HEAD}`),
    );
    assert.equal(status.body.state, "failure");
    assert.equal(status.body.context, OPENAI_ELIGIBILITY_CONTEXT);
  }
});

test("status publication failure keeps the publisher job failed", async () => {
  const result = await runPublisher({
    mode: "finish",
    statusPostFailure: true,
  });
  assert.equal(result.exitCode, 1);
  assert.match(result.stderr, /fixture publication failure/);
  const attemptedStatus = result.requests.find((request) =>
    request.method === "POST" && request.url.endsWith(`/statuses/${HEAD}`),
  );
  assert.equal(attemptedStatus.body.state, "success");
});

test("API failure fails closed without writing fabricated outputs", async () => {
  const result = await runPublisher({mode: "start", apiFailure: true});
  assert.equal(result.exitCode, 1);
  assert.equal(result.outputs, "");
  assert.match(result.stderr, /fixture failure/);
});
