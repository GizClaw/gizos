const DIRECT_USER = /^@[a-z\d](?:[a-z\d-]{0,37}[a-z\d])?$/i;
const DECISIVE_REVIEW_STATES = new Set([
  "APPROVED",
  "CHANGES_REQUESTED",
  "DISMISSED",
]);

export class PolicyError extends Error {}

function globToRegex(pattern) {
  let source = "";
  for (let index = 0; index < pattern.length; index += 1) {
    const character = pattern[index];
    if (character === "*") {
      if (pattern[index + 1] === "*") {
        index += 1;
        if (pattern[index + 1] === "/") {
          index += 1;
          source += "(?:.*/)?";
        } else {
          source += ".*";
        }
      } else {
        source += "[^/]*";
      }
    } else if (character === "?") {
      source += "[^/]";
    } else {
      source += character.replace(/[\\^$+?.()|{}]/g, "\\$&");
    }
  }
  return source;
}

function compilePattern(rawPattern, lineNumber) {
  if (
    rawPattern.startsWith("!") ||
    rawPattern.includes("[") ||
    rawPattern.includes("]") ||
    rawPattern.includes("\\")
  ) {
    throw new PolicyError(
      `unsupported CODEOWNERS pattern on line ${lineNumber}: ${rawPattern}`,
    );
  }

  const anchored = rawPattern.startsWith("/");
  const pattern = rawPattern.replace(/^\/+/, "").replace(/\/+$/, "");
  if (!pattern || pattern.includes("//")) {
    throw new PolicyError(
      `invalid CODEOWNERS pattern on line ${lineNumber}: ${rawPattern}`,
    );
  }

  const containsSlash = pattern.includes("/");
  const prefix = anchored || containsSlash ? "^" : "(?:^|.*/)";
  const finalSegment = pattern.split("/").at(-1);
  const directoryCapable =
    rawPattern.endsWith("/") ||
    (!finalSegment.includes("*") && !finalSegment.includes("?"));
  const suffix = directoryCapable ? "(?:/.*)?$" : "$";
  return new RegExp(`${prefix}${globToRegex(pattern)}${suffix}`);
}

export function parseCodeowners(text) {
  const rules = [];
  for (const [index, rawLine] of text.split(/\r?\n/).entries()) {
    const lineNumber = index + 1;
    const line = rawLine.trim();
    if (!line || line.startsWith("#")) {
      continue;
    }

    const [pattern, ...ownerFields] = line.split(/\s+/);
    const inlineComment = ownerFields.findIndex((field) => field.startsWith("#"));
    const owners =
      inlineComment === -1 ? ownerFields : ownerFields.slice(0, inlineComment);
    const directOwners = owners
      .filter((owner) => DIRECT_USER.test(owner))
      .map((owner) => owner.slice(1).toLowerCase());
    const unsupportedOwners = owners.filter((owner) => !DIRECT_USER.test(owner));
    rules.push({
      lineNumber,
      pattern,
      matcher: compilePattern(pattern, lineNumber),
      directOwners,
      unsupportedOwners,
    });
  }
  if (rules.length === 0) {
    throw new PolicyError("CODEOWNERS contains no effective rules");
  }
  return rules;
}

export function normalizePath(path) {
  if (typeof path !== "string") {
    throw new PolicyError("changed path must be a string");
  }
  const normalized = path.replace(/^\/+/, "");
  if (
    !normalized ||
    normalized.endsWith("/") ||
    normalized.split("/").some((part) => !part || part === "." || part === "..")
  ) {
    throw new PolicyError(`invalid changed path: ${path}`);
  }
  return normalized;
}

export function ownersForPath(rules, path) {
  const normalized = normalizePath(path);
  let effectiveRule = null;
  for (const rule of rules) {
    if (rule.matcher.test(normalized)) {
      effectiveRule = rule;
    }
  }
  return effectiveRule;
}

function latestDecisiveReviews(reviews) {
  const sorted = [...reviews].sort((left, right) => {
    const time = String(left.submitted_at ?? "").localeCompare(
      String(right.submitted_at ?? ""),
    );
    return time || Number(left.id ?? 0) - Number(right.id ?? 0);
  });
  const latest = new Map();
  for (const review of sorted) {
    const login = review.user?.login?.toLowerCase();
    const state = String(review.state ?? "").toUpperCase();
    if (login && DECISIVE_REVIEW_STATES.has(state)) {
      latest.set(login, {...review, state});
    }
  }
  return latest;
}

export function changedPaths(files) {
  const paths = [];
  for (const file of files) {
    paths.push(normalizePath(file.filename));
    if (file.status === "renamed") {
      if (!file.previous_filename) {
        throw new PolicyError(
          `renamed path is missing previous_filename: ${file.filename}`,
        );
      }
      paths.push(normalizePath(file.previous_filename));
    }
  }
  return [...new Set(paths)];
}

export function evaluateOwnership({
  rules,
  files,
  reviews,
  author,
  headSha,
}) {
  const normalizedAuthor = String(author ?? "").toLowerCase();
  if (!DIRECT_USER.test(`@${normalizedAuthor}`)) {
    throw new PolicyError(`invalid pull-request author login: ${author}`);
  }
  if (!/^[0-9a-f]{40}$/i.test(String(headSha ?? ""))) {
    throw new PolicyError(`invalid pull-request head SHA: ${headSha}`);
  }

  const paths = changedPaths(files);
  if (paths.length === 0) {
    throw new PolicyError("pull request contains no changed paths");
  }

  const latestReviews = latestDecisiveReviews(reviews);
  const validApprovers = new Set();
  for (const [login, review] of latestReviews) {
    if (
      login !== normalizedAuthor &&
      review.state === "APPROVED" &&
      review.commit_id === headSha
    ) {
      validApprovers.add(login);
    }
  }

  const selfOwned = [];
  const approved = [];
  const approvalApprovers = new Set();
  const blockers = [];
  for (const path of paths) {
    const rule = ownersForPath(rules, path);
    if (!rule) {
      blockers.push(`${path}: no matching CODEOWNERS rule`);
      continue;
    }
    if (rule.unsupportedOwners.length > 0) {
      blockers.push(
        `${path}: line ${rule.lineNumber} has unsupported owners ${rule.unsupportedOwners.join(", ")}`,
      );
      continue;
    }
    if (rule.directOwners.length === 0) {
      blockers.push(`${path}: line ${rule.lineNumber} has no direct user owner`);
      continue;
    }
    const matchingApprovers = rule.directOwners.filter((owner) =>
      validApprovers.has(owner),
    );
    for (const approver of matchingApprovers) {
      approvalApprovers.add(approver);
    }
    if (rule.directOwners.includes(normalizedAuthor)) {
      selfOwned.push(path);
      continue;
    }
    if (matchingApprovers.length > 0) {
      approved.push({path, approvers: matchingApprovers});
      continue;
    }
    blockers.push(
      `${path}: requires current-head approval from ${rule.directOwners.map((owner) => `@${owner}`).join(", ")}`,
    );
  }

  return {
    success: blockers.length === 0,
    approvalApprovers: [...approvalApprovers].sort(),
    selfOwned,
    approved,
    blockers,
    paths,
  };
}

export function formatSummary(
  result,
  {author, headSha},
) {
  const safe = (value, limit = 1000) => {
    const text = String(value).replace(/[\u0000-\u001f\u007f]/g, (character) =>
      JSON.stringify(character).slice(1, -1),
    );
    return text.length <= limit ? text : `${text.slice(0, limit - 3)}...`;
  };
  const lines = [
    `Author: @${safe(author)}`,
    `Head: \`${safe(headSha)}\``,
    `Evaluated paths: ${result.paths.length}`,
    `Self-owned paths: ${result.selfOwned.length}`,
    `Paths satisfied by independent approval: ${result.approved.length}`,
    `Current-head approving owners: ${result.approvalApprovers.length}`,
  ];
  if (result.blockers.length > 0) {
    lines.push("", "Blocking paths:");
    for (const blocker of result.blockers.slice(0, 50)) {
      lines.push(`- ${safe(blocker)}`);
    }
    if (result.blockers.length > 50) {
      lines.push(`- ... ${result.blockers.length - 50} more`);
    }
  }
  return safe(lines.join("\n"), 60000);
}
