import {appendFile, readFile} from "node:fs/promises";
import {execFileSync} from "node:child_process";

import {
  EligibilityStatusError,
  OPENAI_ELIGIBILITY_CONTEXT,
  OWNERSHIP_ELIGIBILITY_CONTEXT,
  assertLatestPendingStatus,
  openAiFinalStatus,
  statusPayload,
  validateSha,
} from "./common.mjs";

const API_VERSION = "2022-11-28";
const APPROVED_FORK_DESCRIPTIONS = new Set([
  "Ownership approved fork checks for this exact head",
  "Ownership passed; approved fork checks were requested",
]);

function requiredEnvironment(name) {
  const value = process.env[name];
  if (!value) {
    throw new EligibilityStatusError(
      `missing required environment variable: ${name}`,
    );
  }
  return value;
}

async function githubRequest(path, {method = "GET", body} = {}) {
  const token = requiredEnvironment("GITHUB_TOKEN");
  const apiUrl = process.env.GITHUB_API_URL ?? "https://api.github.com";
  const response = await fetch(`${apiUrl.replace(/\/$/, "")}${path}`, {
    method,
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": API_VERSION,
      "User-Agent": "gizos-merge-eligibility-status",
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await response.text();
  let payload = null;
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch {
      throw new EligibilityStatusError(
        `GitHub API ${method} ${path} returned invalid JSON (${response.status})`,
      );
    }
  }
  if (!response.ok) {
    throw new EligibilityStatusError(
      `GitHub API ${method} ${path} failed (${response.status}): ${payload?.message ?? text}`,
    );
  }
  return payload;
}

async function publishStatus(repository, sha, status) {
  return githubRequest(`/repos/${repository}/statuses/${validateSha(sha)}`, {
    method: "POST",
    body: status,
  });
}

async function assertCurrentGeneration(repository, sha, runUrl) {
  const statuses = await githubRequest(
    `/repos/${repository}/commits/${validateSha(sha)}/statuses?per_page=100`,
  );
  assertLatestPendingStatus(statuses, {
    context: OPENAI_ELIGIBILITY_CONTEXT,
    runUrl,
  });
}

async function assertApprovedForkReview(repository, headSha) {
  const statuses = await githubRequest(
    `/repos/${repository}/commits/${validateSha(headSha)}/statuses?per_page=100`,
  );
  const ownership = statuses.find(
    (status) => status.context === OWNERSHIP_ELIGIBILITY_CONTEXT,
  );
  if (
    ownership?.state !== "success" ||
    !APPROVED_FORK_DESCRIPTIONS.has(ownership.description)
  ) {
    throw new EligibilityStatusError(
      "fork OpenAI review dispatch lacks current-head ownership approval",
    );
  }
}

function pullRequestNumber(event) {
  if (event.issue?.pull_request && event.issue.number) {
    return event.issue.number;
  }
  if (process.env.GITHUB_EVENT_NAME === "workflow_dispatch") {
    const number = Number(requiredEnvironment("PULL_REQUEST_NUMBER"));
    if (Number.isSafeInteger(number) && number > 0) {
      return number;
    }
  }
  throw new EligibilityStatusError(
    "event does not identify a pull request eligible for OpenAI review",
  );
}

function assertTrustedCheckout(expectedSha) {
  const checkedOutSha = execFileSync("git", ["rev-parse", "HEAD"], {
    encoding: "utf8",
  }).trim();
  if (checkedOutSha !== expectedSha) {
    throw new EligibilityStatusError(
      `trusted checkout mismatch: expected ${expectedSha}, received ${checkedOutSha}`,
    );
  }
}

async function writeOutputs(values) {
  const outputPath = requiredEnvironment("GITHUB_OUTPUT");
  const lines = Object.entries(values).map(([name, value]) => `${name}=${value}`);
  await appendFile(outputPath, `${lines.join("\n")}\n`);
}

async function start(repository, event, runUrl) {
  const expectedTrustedSha = validateSha(
    requiredEnvironment("TRUSTED_SOURCE_SHA"),
    "trusted source SHA",
  );
  assertTrustedCheckout(expectedTrustedSha);
  const number = pullRequestNumber(event);
  const pullRequest = await githubRequest(`/repos/${repository}/pulls/${number}`);
  if (
    pullRequest.state !== "open" ||
    pullRequest.draft ||
    pullRequest.base?.ref !== "main"
  ) {
    throw new EligibilityStatusError(
      `pull request #${number} is not an open, ready main-branch pull request`,
    );
  }
  const headSha = validateSha(pullRequest.head?.sha, "pull-request head SHA");
  const baseSha = validateSha(pullRequest.base?.sha, "pull-request base SHA");
  const isFork = pullRequest.head?.repo?.full_name !== repository;
  if (isFork && process.env.GITHUB_EVENT_NAME !== "workflow_dispatch") {
    throw new EligibilityStatusError(
      "fork pull requests require current-head CODEOWNER approval before OpenAI review",
    );
  }
  if (process.env.GITHUB_EVENT_NAME === "workflow_dispatch") {
    const expectedHeadSha = validateSha(
      requiredEnvironment("EXPECTED_HEAD_SHA"),
      "approved fork head SHA",
    );
    if (!isFork || headSha !== expectedHeadSha) {
      throw new EligibilityStatusError(
        `approved fork review identity does not match pull request #${number}`,
      );
    }
    await assertApprovedForkReview(repository, headSha);
  }
  await publishStatus(
    repository,
    headSha,
    statusPayload({
      state: "pending",
      context: OPENAI_ELIGIBILITY_CONTEXT,
      description: "OpenAI review policy is evaluating this exact head",
      targetUrl: runUrl,
    }),
  );
  await writeOutputs({
    pull_request_number: pullRequest.number,
    base_sha: baseSha,
    head_sha: headSha,
  });
}

async function finish(repository, runUrl) {
  const number = Number(requiredEnvironment("PULL_REQUEST_NUMBER"));
  if (!Number.isSafeInteger(number) || number < 1) {
    throw new EligibilityStatusError(
      `invalid pull-request number: ${process.env.PULL_REQUEST_NUMBER}`,
    );
  }
  const expectedHeadSha = validateSha(
    requiredEnvironment("EXPECTED_HEAD_SHA"),
    "expected head SHA",
  );
  const expectedBaseSha = validateSha(
    requiredEnvironment("EXPECTED_BASE_SHA"),
    "expected base SHA",
  );
  const expectedWorkflowSourceSha = validateSha(
    requiredEnvironment("EXPECTED_REVIEW_WORKFLOW_SHA"),
    "expected review workflow source SHA",
  );
  const pullRequest = await githubRequest(`/repos/${repository}/pulls/${number}`);
  const currentHeadSha = validateSha(
    pullRequest.head?.sha,
    "current pull-request head SHA",
  );
  const currentBaseSha = validateSha(
    pullRequest.base?.sha,
    "current pull-request base SHA",
  );
  if (
    pullRequest.state !== "open" ||
    currentHeadSha !== expectedHeadSha ||
    currentBaseSha !== expectedBaseSha
  ) {
    await publishStatus(
      repository,
      expectedHeadSha,
      statusPayload({
        state: "failure",
        context: OPENAI_ELIGIBILITY_CONTEXT,
        description: "OpenAI review result is stale for the current PR identity",
        targetUrl: runUrl,
      }),
    );
    throw new EligibilityStatusError(
      `pull request #${number} changed before OpenAI eligibility publication`,
    );
  }

  await assertCurrentGeneration(repository, expectedHeadSha, runUrl);
  const finalStatus = openAiFinalStatus({
    reviewResult: process.env.REVIEW_RESULT,
    readinessEvidence: process.env.READINESS_EVIDENCE,
    expectedRepository: repository,
    expectedPullRequestNumber: number,
    expectedBaseSha,
    expectedHeadSha,
    expectedWorkflowSourceSha,
  });
  await publishStatus(
    repository,
    expectedHeadSha,
    statusPayload({
      state: finalStatus.state,
      description: finalStatus.description,
      context: OPENAI_ELIGIBILITY_CONTEXT,
      targetUrl: runUrl,
    }),
  );
  if (!finalStatus.publisherSucceeded) {
    process.exitCode = 1;
  }
}

async function main() {
  const repository = requiredEnvironment("GITHUB_REPOSITORY");
  const eventPath = requiredEnvironment("GITHUB_EVENT_PATH");
  const runUrl = requiredEnvironment("GITHUB_RUN_URL");
  const mode = requiredEnvironment("ELIGIBILITY_STATUS_MODE");
  const event = JSON.parse(await readFile(eventPath, "utf8"));
  if (mode === "start") {
    await start(repository, event, runUrl);
  } else if (mode === "finish") {
    await finish(repository, runUrl);
  } else {
    throw new EligibilityStatusError(`invalid eligibility status mode: ${mode}`);
  }
}

try {
  await main();
} catch (error) {
  const message = error instanceof Error ? error.message : String(error);
  console.error(message.replace(/[\u0000-\u001f\u007f]/g, " ").slice(0, 4000));
  process.exitCode = 1;
}
