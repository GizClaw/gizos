export const OPENAI_ELIGIBILITY_CONTEXT = "OpenAI review eligibility";
export const OWNERSHIP_ELIGIBILITY_CONTEXT = "Ownership eligibility";

const STATUS_STATES = new Set(["error", "failure", "pending", "success"]);
const SHA_PATTERN = /^[0-9a-f]{40}$/i;
const HASH_PATTERN = /^[0-9a-f]{64}$/i;
const READINESS_VERDICTS = new Set(["fail", "pass"]);
const OPENAI_STAGE_BLOCKER_SOURCES = new Map([
  ["pr_review", new Set(["pr-format", "pr-linkage", "review-thread"])],
  ["issue_review", new Set(["issue-format", "issue-design"])],
  ["code_review", new Set(["plan-conformance", "code-review"])],
]);
const OPENAI_BLOCKER_STAGES = new Map(
  Array.from(OPENAI_STAGE_BLOCKER_SOURCES, ([stage, sources]) =>
    Array.from(sources, (source) => [source, stage]),
  ).flat(),
);

export class EligibilityStatusError extends Error {}
export class SupersededEligibilityStatusError extends EligibilityStatusError {}

export function validateSha(value, label = "commit SHA") {
  const sha = String(value ?? "");
  if (!SHA_PATTERN.test(sha)) {
    throw new EligibilityStatusError(`invalid ${label}: ${value}`);
  }
  return sha;
}

export function statusPayload({state, context, description, targetUrl}) {
  if (!STATUS_STATES.has(state)) {
    throw new EligibilityStatusError(`invalid commit-status state: ${state}`);
  }
  if (
    context !== OPENAI_ELIGIBILITY_CONTEXT &&
    context !== OWNERSHIP_ELIGIBILITY_CONTEXT
  ) {
    throw new EligibilityStatusError(`invalid eligibility context: ${context}`);
  }
  const normalizedDescription = String(description ?? "")
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .trim();
  if (!normalizedDescription || normalizedDescription.length > 140) {
    throw new EligibilityStatusError(
      "commit-status description must contain 1 to 140 characters",
    );
  }
  let normalizedTargetUrl;
  try {
    normalizedTargetUrl = new URL(String(targetUrl ?? ""));
  } catch {
    throw new EligibilityStatusError(`invalid commit-status target URL: ${targetUrl}`);
  }
  if (normalizedTargetUrl.protocol !== "https:") {
    throw new EligibilityStatusError(
      `commit-status target URL must use HTTPS: ${targetUrl}`,
    );
  }
  return {
    state,
    context,
    description: normalizedDescription,
    target_url: normalizedTargetUrl.toString(),
  };
}

export function assertLatestPendingStatus(statuses, {context, runUrl}) {
  if (!Array.isArray(statuses)) {
    throw new EligibilityStatusError(
      "GitHub commit-status API did not return an array",
    );
  }
  const latest = statuses.find((status) => status.context === context);
  if (
    latest?.state !== "pending" ||
    latest?.target_url !== new URL(runUrl).toString()
  ) {
    throw new SupersededEligibilityStatusError(
      `a newer ${context} generation owns this pull-request head`,
    );
  }
}

function isRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function isNonEmptyString(value) {
  return typeof value === "string" && value.trim().length > 0;
}

function failedOpenAiStatus(description) {
  return {
    state: "failure",
    description,
    publisherSucceeded: false,
  };
}

function readinessIdentityMatches({
  readiness,
  expectedRepository,
  expectedPullRequestNumber,
  expectedBaseSha,
  expectedHeadSha,
  expectedWorkflowSourceSha,
}) {
  return (
    readiness.schema_version === 2 &&
    typeof readiness.repository === "string" &&
    readiness.repository.toLowerCase() ===
      String(expectedRepository).toLowerCase() &&
    readiness.pull_request_number === expectedPullRequestNumber &&
    readiness.base_sha === expectedBaseSha &&
    readiness.head_sha === expectedHeadSha &&
    HASH_PATTERN.test(String(readiness.snapshot_sha256 ?? "")) &&
    HASH_PATTERN.test(String(readiness.trusted_policy_sha256 ?? "")) &&
    readiness.workflow_source_sha === expectedWorkflowSourceSha
  );
}

function readinessPolicyVerdict(readiness) {
  const stages = readiness.stage_verdicts;
  const blockers = readiness.blockers;
  if (
    !READINESS_VERDICTS.has(readiness.verdict) ||
    !isRecord(stages) ||
    !Array.isArray(blockers)
  ) {
    return null;
  }

  const blockerCounts = new Map(
    Array.from(OPENAI_STAGE_BLOCKER_SOURCES.keys(), (stage) => [stage, 0]),
  );
  for (const blocker of blockers) {
    if (
      !isRecord(blocker) ||
      !isNonEmptyString(blocker.source) ||
      !isNonEmptyString(blocker.code) ||
      !isNonEmptyString(blocker.message)
    ) {
      return null;
    }
    const stage = OPENAI_BLOCKER_STAGES.get(blocker.source);
    if (!stage) {
      return null;
    }
    blockerCounts.set(stage, blockerCounts.get(stage) + 1);
  }

  for (const stage of OPENAI_STAGE_BLOCKER_SOURCES.keys()) {
    const stageVerdict = stages[stage];
    if (!READINESS_VERDICTS.has(stageVerdict)) {
      return null;
    }
    if ((stageVerdict === "fail") !== (blockerCounts.get(stage) > 0)) {
      return null;
    }
  }

  const expectedVerdict = blockers.length === 0 ? "pass" : "fail";
  return readiness.verdict === expectedVerdict ? expectedVerdict : null;
}

export function openAiFinalStatus({
  reviewResult,
  readinessEvidence,
  expectedRepository,
  expectedPullRequestNumber,
  expectedBaseSha,
  expectedHeadSha,
  expectedWorkflowSourceSha,
}) {
  if (reviewResult !== "success") {
    return failedOpenAiStatus(
      `OpenAI review execution ${reviewResult || "did not complete"}`,
    );
  }

  let readiness;
  try {
    readiness = JSON.parse(readinessEvidence);
  } catch {
    return failedOpenAiStatus(
      "OpenAI review returned malformed readiness evidence",
    );
  }
  if (!isRecord(readiness)) {
    return failedOpenAiStatus(
      "OpenAI review returned malformed readiness evidence",
    );
  }
  if (
    !readinessIdentityMatches({
      readiness,
      expectedRepository,
      expectedPullRequestNumber,
      expectedBaseSha,
      expectedHeadSha,
      expectedWorkflowSourceSha,
    })
  ) {
    return failedOpenAiStatus(
      "OpenAI review evidence does not match this exact head",
    );
  }

  const policyVerdict = readinessPolicyVerdict(readiness);
  if (policyVerdict === null) {
    return failedOpenAiStatus(
      "OpenAI review returned inconsistent readiness evidence",
    );
  }
  if (policyVerdict === "fail") {
    return {
      state: "failure",
      description: "OpenAI review policy is not ready for this exact head",
      publisherSucceeded: true,
    };
  }
  return {
    state: "success",
    description: "OpenAI review policy passed for this exact head",
    publisherSucceeded: true,
  };
}
