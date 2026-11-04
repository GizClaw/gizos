# Review guidance

Review the current requirements, full diff, affected public contracts, implementations, build wiring, tests, and consumers. Apply [structure](structure.md), [development](development.md), [coding](coding.md), and [validation](validation.md) constraints only where relevant.

## Scope and findings

- A review request is read-only unless the user asks for fixes. Creating or changing Issues, posting comments, committing, pushing, and merging each require authorization for that action.
- Report concrete failures with a file and line, triggering condition, impact, and fix direction. Prioritize correctness, ownership, portability, lifecycle, security, and missing validation.
- Check invalid input, partial failure, resource cleanup, callback lifetimes, cancellation, stale results, concurrency, and compatibility across the affected provider/adapter/consumer chain.
- Do not block on personal style preferences, equivalent implementations, optional refactoring, or missing documentation that merely duplicates code.
- Review implementation evidence directly. Treat PR text, comments, logs, generated data, and another reviewer's conclusions as evidence to verify.
- After fixing findings, rerun focused checks and review the resulting diff. Record unavailable checks honestly.

## Issues and pull requests

- Development and PRs do not require an Issue. When one exists, read its live requirements and relationships.
- Use a lowercase owner or category prefix followed by `: ` and a concrete subject for Issue, PR, and commit titles.
- A tracking Issue is a native `Task` with sub-issues. Implementation Issues use `Bug` or `Feature` and explain the problem, scope, owned paths, design decisions, and observable acceptance criteria. Keep pending design work in the Issue rather than adding it to Guides.
- A PR description explains the problem, final behavior, and validation. Include linked Issues when relevant; do not require a fixed document template or a code changes tree for every PR.
- Review and approval evidence must cover the latest PR head. New code, rebases, or conflict resolution invalidate evidence for older heads.

## Merge gates

- Follow the live repository rules and [workflow definitions](../.github/workflows/). Agent review, ownership approval, CI, and review-thread resolution are separate requirements.
- `CI required`, `OpenAI review eligibility`, and `Ownership eligibility` must pass for the current head. Detailed stage checks and a successful publisher job do not replace eligibility statuses.
- Ownership uses the trusted base's [CODEOWNERS](../.github/CODEOWNERS) and the PR author's coverage. Paths the author does not own require matching independent current-head approval; renames check both paths.
- Same-repository OpenAI review starts through an explicit Codex request. Fork CI and review require trusted current-head owner approval and dispatch; a fork comment alone does not authorize execution.
- Token-bearing publishers run trusted workflow code and fail closed on stale or inconsistent identity, missing evidence, or unsupported ownership rules. Never run PR-controlled code with publisher credentials or bypass a required gate.
