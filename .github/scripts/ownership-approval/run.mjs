import {appendFile, readFile} from "node:fs/promises";
import {execFileSync} from "node:child_process";

import {
  OWNERSHIP_ELIGIBILITY_CONTEXT,
  SupersededEligibilityStatusError,
  assertLatestPendingStatus,
  statusPayload,
  validateSha,
} from "../merge-eligibility-status/common.mjs";
import {
  evaluateOwnership,
  formatSummary,
  parseCodeowners,
  PolicyError,
} from "./common.mjs";

const API_VERSION = "2022-11-28";
const FORK_CHECKS_AUTHORIZATION_DESCRIPTION =
  "Ownership approved fork checks for this exact head";
const FORK_CHECKS_DISPATCH_DESCRIPTION =
  "Ownership passed; approved fork checks were requested";

function requiredEnvironment(name) {
  const value = process.env[name];
  if (!value) {
    throw new Error(`missing required environment variable: ${name}`);
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
      "User-Agent": "gizos-ownership-approval",
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await response.text();
  let payload = null;
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch {
      throw new Error(
        `GitHub API ${method} ${path} returned invalid JSON (${response.status})`,
      );
    }
  }
  if (!response.ok) {
    throw new Error(
      `GitHub API ${method} ${path} failed (${response.status}): ${payload?.message ?? text}`,
    );
  }
  return payload;
}

async function paginated(path) {
  const values = [];
  for (let page = 1; ; page += 1) {
    const separator = path.includes("?") ? "&" : "?";
    const payload = await githubRequest(
      `${path}${separator}per_page=100&page=${page}`,
    );
    if (!Array.isArray(payload)) {
      throw new Error(`GitHub API ${path} did not return an array`);
    }
    values.push(...payload);
    if (payload.length < 100) {
      return values;
    }
    if (page >= 100) {
      throw new Error(`GitHub API pagination exceeded 100 pages for ${path}`);
    }
  }
}

async function publishStatus(repository, headSha, status) {
  return githubRequest(
    `/repos/${repository}/statuses/${validateSha(headSha, "pull-request head SHA")}`,
    {method: "POST", body: status},
  );
}

async function assertCurrentGeneration(repository, headSha, runUrl) {
  const statuses = await githubRequest(
    `/repos/${repository}/commits/${validateSha(headSha)}/statuses?per_page=100`,
  );
  assertLatestPendingStatus(statuses, {
    context: OWNERSHIP_ELIGIBILITY_CONTEXT,
    runUrl,
  });
  return statuses;
}

async function dispatchApprovedForkChecks(
  repository,
  baseRef,
  headSha,
  pullRequestNumber,
) {
  await githubRequest(`/repos/${repository}/actions/workflows/ci.yml/dispatches`, {
    method: "POST",
    body: {
      ref: baseRef,
      inputs: {checkout_sha: validateSha(headSha, "fork CI checkout SHA")},
    },
  });
  await githubRequest(
    `/repos/${repository}/actions/workflows/openai-pr-review.yml/dispatches`,
    {
      method: "POST",
      body: {
        ref: baseRef,
        inputs: {
          pull_request_number: String(pullRequestNumber),
          head_sha: validateSha(headSha, "fork review head SHA"),
        },
      },
    },
  );
}

async function writeStepSummary(title, summary) {
  const path = process.env.GITHUB_STEP_SUMMARY;
  if (path) {
    await appendFile(path, `## ${title}\n\n${summary}\n`);
  }
}

async function writeOutput(name, value) {
  const path = process.env.GITHUB_OUTPUT;
  if (path) {
    await appendFile(path, `${name}=${value}\n`);
  }
}

function eventPullRequest(event) {
  let pullRequest = event.pull_request;
  if (event.workflow_run) {
    if (
      event.workflow_run.name !== "Ownership Approval Review Request" ||
      event.workflow_run.event !== "pull_request_review" ||
      event.workflow_run.conclusion !== "success"
    ) {
      throw new Error(
        "workflow_run is not a successful ownership review request",
      );
    }
    if (event.workflow_run.pull_requests?.length !== 1) {
      throw new Error("workflow_run does not identify exactly one pull request");
    }
    [pullRequest] = event.workflow_run.pull_requests;
  }
  if (!pullRequest?.number || !pullRequest.head?.sha || !pullRequest.base?.sha) {
    throw new Error("event does not contain a complete pull_request snapshot");
  }
  return pullRequest;
}

async function main() {
  const repository = requiredEnvironment("GITHUB_REPOSITORY");
  const eventPath = requiredEnvironment("GITHUB_EVENT_PATH");
  const runUrl = requiredEnvironment("GITHUB_RUN_URL");
  const event = JSON.parse(await readFile(eventPath, "utf8"));
  const eventPr = eventPullRequest(event);
  const headSha = validateSha(eventPr.head.sha, "event head SHA");
  const pendingStatus = statusPayload({
    state: "pending",
    context: OWNERSHIP_ELIGIBILITY_CONTEXT,
    description: "Ownership policy is evaluating this exact head",
    targetUrl: runUrl,
  });

  try {
    await publishStatus(repository, headSha, pendingStatus);
    const currentPr = await githubRequest(
      `/repos/${repository}/pulls/${eventPr.number}`,
    );

    const checkedOutSha = execFileSync("git", ["rev-parse", "HEAD"], {
      encoding: "utf8",
    }).trim();
    if (checkedOutSha !== eventPr.base.sha) {
      throw new PolicyError(
        `trusted checkout mismatch: expected ${eventPr.base.sha}, received ${checkedOutSha}`,
      );
    }
    if (currentPr.state !== "open") {
      throw new PolicyError(`pull request #${eventPr.number} is not open`);
    }
    if (
      currentPr.head.sha !== eventPr.head.sha ||
      currentPr.base.sha !== eventPr.base.sha ||
      currentPr.base.ref !== "main" ||
      (eventPr.user?.login &&
        currentPr.user.login.toLowerCase() !== eventPr.user.login.toLowerCase())
    ) {
      throw new PolicyError(
        "event pull-request identity is stale; wait for the current PR event to rerun",
      );
    }

    const [files, reviews, codeownersText] = await Promise.all([
      paginated(`/repos/${repository}/pulls/${eventPr.number}/files`),
      paginated(`/repos/${repository}/pulls/${eventPr.number}/reviews`),
      readFile(".github/CODEOWNERS", "utf8"),
    ]);
    if (files.length !== currentPr.changed_files) {
      throw new PolicyError(
        `changed-file evidence is incomplete: expected ${currentPr.changed_files}, received ${files.length}`,
      );
    }

    const rules = parseCodeowners(codeownersText);
    const result = evaluateOwnership({
      rules,
      files,
      reviews,
      author: currentPr.user.login,
      headSha: currentPr.head.sha,
    });
    const summary = formatSummary(result, {
      author: currentPr.user.login,
      headSha: currentPr.head.sha,
    });
    const statuses = await assertCurrentGeneration(repository, headSha, runUrl);
    if (!currentPr.head.repo?.full_name) {
      throw new PolicyError("pull-request head repository identity is missing");
    }
    const isFork = currentPr.head.repo?.full_name !== repository;
    const hasApprovedForkChecks =
      isFork && result.success && result.approvalApprovers.length > 0;
    const forkChecksAlreadyDispatched = statuses.some(
      (status) =>
        status.context === OWNERSHIP_ELIGIBILITY_CONTEXT &&
        status.state === "success" &&
        status.description === FORK_CHECKS_DISPATCH_DESCRIPTION,
    );
    await writeStepSummary(
      result.success
        ? "Ownership approval policy satisfied"
        : "Ownership approval required",
      summary,
    );
    const finalDescription = result.success
      ? hasApprovedForkChecks
        ? forkChecksAlreadyDispatched
          ? FORK_CHECKS_DISPATCH_DESCRIPTION
          : FORK_CHECKS_AUTHORIZATION_DESCRIPTION
        : "Ownership policy passed for this exact head"
      : "Ownership approval is required for this exact head";
    await publishStatus(
      repository,
      headSha,
      statusPayload({
        state: result.success ? "success" : "failure",
        context: OWNERSHIP_ELIGIBILITY_CONTEXT,
        description: finalDescription,
        targetUrl: runUrl,
      }),
    );
    if (hasApprovedForkChecks && !forkChecksAlreadyDispatched) {
      await dispatchApprovedForkChecks(
        repository,
        currentPr.base.ref,
        headSha,
        currentPr.number,
      );
      await publishStatus(
        repository,
        headSha,
        statusPayload({
          state: "success",
          context: OWNERSHIP_ELIGIBILITY_CONTEXT,
          description: FORK_CHECKS_DISPATCH_DESCRIPTION,
          targetUrl: runUrl,
        }),
      );
    }
    await writeOutput("head_sha", headSha);
    await writeOutput("eligible", result.success ? "true" : "false");
  } catch (error) {
    const message = (error instanceof Error ? error.message : String(error))
      .replace(/[\u0000-\u001f\u007f]/g, " ")
      .slice(0, 4000);
    try {
      await assertCurrentGeneration(repository, headSha, runUrl);
      await publishStatus(
        repository,
        headSha,
        statusPayload({
          state: "failure",
          context: OWNERSHIP_ELIGIBILITY_CONTEXT,
          description: "Ownership policy failed to produce trusted evidence",
          targetUrl: runUrl,
        }),
      );
    } catch (publicationError) {
      const publicationMessage =
        publicationError instanceof Error
          ? publicationError.message
          : String(publicationError);
      console.error(publicationMessage);
      if (publicationError instanceof SupersededEligibilityStatusError) {
        console.error("the newer ownership generation was left unchanged");
      }
    }
    await writeStepSummary(
      "Ownership approval evaluation failed closed",
      `The policy could not produce trusted evidence.\n\n- ${message}`,
    );
    await writeOutput("eligible", "false");
    process.exitCode = 1;
  }
}

await main();
