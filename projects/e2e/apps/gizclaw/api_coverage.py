"""Fail-closed GizClaw API coverage audit for one complete Desktop E2E log.

The matrix is a requirement, not evidence. A symbol in a header, a mock test,
or a successful RPC return without its business assertion is not live coverage.
This auditor checks logs; the caller still owns provenance of the executed binary.
Telemetry is one-way: its assertion covers transport acceptance only, never
server acknowledgement or persistence, matching that API's explicit contract.
"""

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import sys


PREFIX = "h2_gizclaw_"
TOP_CASES = {"connectivity", "rpc", "firmware", "voice", "concurrency", "service"}
RPC_CASES = {"profile", "catalog-workspace", "speech", "workspace-reconnect",
             "contact", "friend", "group", "gameplay", "peer-name-isolation",
             "telemetry"}
CASES = TOP_CASES | {"rpc/" + name for name in RPC_CASES}


@dataclass(frozen=True)
class Rule:
    symbol: str
    case: str
    calls: tuple[str, ...]
    assertion_symbol: str
    assertion_stage: str


def requirements():
    """Enumerate all approved methods independently of the header inventory.

    Uninstrumented assertion stages intentionally remain requirements and fail
    the audit. Do not replace them with 'case passed' or source-name matching.
    """
    groups = {
        "connectivity": "ping speedtest register peer_delete",
        "firmware": "firmware_get",
        "rpc/profile": "profile_get profile_put_name profile_put_emoji",
        "rpc/catalog-workspace": (
            "workflow_list workflow_get workspace_list workspace_get "
            "workspace_create workspace_set_input workspace_delete "
            "workspace_activate workspace_reload workspace_history_list"),
        "rpc/contact": "contact_list contact_get contact_create contact_put contact_delete",
        "rpc/friend": (
            "friend_list friend_info_get friend_add friend_delete "
            "friend_invite_token_get friend_invite_token_create friend_invite_token_clear"),
        "rpc/group": (
            "friend_group_list friend_group_get friend_group_create friend_group_put "
            "friend_group_delete friend_group_join friend_group_invite_token_get "
            "friend_group_invite_token_create friend_group_invite_token_clear "
            "friend_group_member_list friend_group_member_put friend_group_member_delete"),
        "rpc/gameplay": (
            "pet_list pet_get pet_adopt pet_delete pet_drive pet_pixa_download "
            "pet_action_get point_get point_transaction_list"),
        "rpc/telemetry": "telemetry_send",
        "rpc/speech": "speech_transcribe speech_extract",
    }
    rules = []
    for case, methods in groups.items():
        for method in methods.split():
            create = PREFIX + "req_create_" + method
            parse = PREFIX + "resp_parse_" + method
            stage = ("profile-assert" if method.startswith("profile_") else
                     "workflow-assert" if method.startswith("workflow_") else
                     method + "-assert")
            calls = (create, PREFIX + "req_do", PREFIX + "req_wait", parse)
            if method == "speedtest":
                calls = (create, PREFIX + "req_do", PREFIX + "service_poll",
                         PREFIX + "req_wait", parse)
            rules += [Rule(symbol, case, calls, parse, stage) for symbol in (create, parse)]
            if not method.startswith("speech_"):
                rpc = PREFIX + "rpc_" + method
                rules.append(Rule(rpc, case, (rpc,), rpc, stage))
    for method in "init start set_track unset_track audio_start audio_end poll stop deinit".split():
        symbol = PREFIX + "service_" + method
        case = "voice" if method in {"set_track", "unset_track", "audio_start", "audio_end"} else "service"
        rules.append(Rule(symbol, case, (symbol,), symbol, "service_" + method + "-assert"))
    for method in "create destroy write read".split():
        symbol = PREFIX + "pcm_track_" + method
        rules.append(Rule(symbol, "voice", (symbol,), symbol, "pcm_track_" + method + "-assert"))
    for method in "do wait cancel release".split():
        symbol = PREFIX + "req_" + method
        rules.append(Rule(symbol, "service", (symbol,), symbol, "req_" + method + "-assert"))
    for method in "create cancel release".split():
        symbol = PREFIX + "conversation_" + method
        rules.append(Rule(symbol, "voice", (symbol,), symbol, "conversation_" + method + "-assert"))
    symbol = PREFIX + "req_create_audio_play"
    rules.append(Rule(symbol, "voice", (symbol, PREFIX + "req_do", PREFIX + "req_wait"),
                      symbol, "audio_play-assert"))
    return sorted(rules, key=lambda rule: rule.symbol)


def validate_inventory(rules, text):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    inventory = re.findall(r"H2_GIZCLAW_API\((h2_gizclaw_\w+)\)", text)
    names = [rule.symbol for rule in rules]
    if (len(inventory) != 181 or len(set(inventory)) != 181 or
            len(names) != 181 or len(set(names)) != 181 or set(names) != set(inventory)):
        raise ValueError("coverage matrix does not match the approved 181-function inventory")
    if any(rule.case not in CASES for rule in rules):
        raise ValueError("coverage matrix references an unknown case")


def _fields(line):
    fields = {}
    for token in line.split():
        if "=" not in token:
            raise ValueError("invalid evidence field")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError("empty or duplicate evidence field")
        fields[key] = value
    return fields


def audit(lines, rules, *, endpoint, backend, profile, platform, process_exit_code):
    """Only report call/proof line numbers, never echo arbitrary log contents."""
    spans, stack, issues, summaries = {}, [], [], []
    if process_exit_code != 0:
        issues.append("test process did not exit successfully")
    digest = hashlib.sha256()
    for lineno, line in enumerate(lines, 1):
        digest.update(line.encode("utf-8"))
        if not line.startswith("H2_GIZCLAW_E2E "):
            continue
        # Other E2E records (including free text) are not coverage evidence.
        if not ("stage=coverage-" in line or "stage=summary " in line or "symbol=" in line):
            continue
        try:
            fields = _fields(line[len("H2_GIZCLAW_E2E "):])
            stage = fields["stage"]
            if summaries:
                raise ValueError("evidence after final summary")
            if stage == "coverage-begin":
                case = fields["case"]
                parent = case.rpartition("/")[0]
                if (case not in CASES or case in spans or
                        parent != (stack[-1] if stack else "")):
                    raise ValueError("duplicate, unknown, or incorrectly nested case")
                spans[case] = {"events": [], "terminal": None}
                stack.append(case)
            elif stage == "coverage-end":
                case = fields["case"]
                if not stack or stack[-1] != case:
                    raise ValueError("case terminal without matching open case")
                stack.pop()
                spans[case]["terminal"] = (
                    fields["status"] == "PASS" and fields["rc"] == "0" and
                    fields["cleanup_rc"] == "0")
            elif stage == "summary":
                if stack:
                    raise ValueError("final summary before all cases closed")
                summaries.append(fields)
            elif "symbol" in fields and fields["symbol"].startswith(PREFIX):
                if not stack:
                    raise ValueError("API evidence outside a case")
                if fields["result"] not in {"PASS", "FAIL"}:
                    raise ValueError("invalid API result")
                rc = int(fields["rc"])
                if (rc == 0) != (fields["result"] == "PASS"):
                    raise ValueError("API result disagrees with return code")
                spans[stack[-1]]["events"].append((lineno, fields["symbol"], stage, rc))
        except (KeyError, ValueError):
            # The exception text is deliberately not derived from log values.
            issues.append(f"line {lineno}: malformed or out-of-order coverage evidence")
    if stack:
        issues.append("unterminated case boundaries")
    if set(spans) != CASES:
        issues.append("missing required case boundaries")
    if any(span["terminal"] is not True for span in spans.values()):
        issues.append("a case failed, was skipped, or lacks a unique passing terminal")
    if len(summaries) != 1:
        issues.append("expected exactly one final Desktop summary")
    else:
        expected = dict(endpoint=endpoint, backend=backend, profile=profile, platform=platform,
                        suite="all", selected="6", terminal="6", **{"pass": "6"},
                        fail="0", error="0", blocked="0", cancelled="0", cleanup_rc="0",
                        retained_resources="0", complete="true", exit_code="0",
                        first_failure_case="-", first_failure_rc="0")
        if any(summaries[0].get(key) != value for key, value in expected.items()):
            issues.append("Desktop summary failed or run identity differs from requested acceptance lane")
    results = []
    for rule in rules:
        span = spans.get(rule.case, {})
        events = span.get("events", [])
        proof_line, call_lines = None, []
        # Find an actual ordered call chain, then an explicit business assertion.
        for lineno, symbol, stage, rc in events:
            if rc != 0:
                if symbol in rule.calls:
                    call_lines = []
                continue
            if (symbol == rule.assertion_symbol and stage == rule.assertion_stage and
                    len(call_lines) == len(rule.calls)):
                proof_line = lineno
                break
            if symbol == rule.calls[0] and not stage.endswith("-assert"):
                call_lines = [lineno]
            elif (len(call_lines) < len(rule.calls) and symbol == rule.calls[len(call_lines)]
                    and not stage.endswith("-assert")):
                call_lines.append(lineno)
        parent = rule.case.split("/")[0]
        case_ok = span.get("terminal") is True and spans.get(parent, {}).get("terminal") is True
        passed = proof_line is not None and case_ok
        results.append({**asdict(rule), "status": "covered" if passed else "missing",
                        "call_lines": call_lines, "assertion_line": proof_line})
    covered = sum(row["status"] == "covered" for row in results)
    return {"valid": not issues and covered == len(rules), "log_sha256": digest.hexdigest(),
            "required": len(rules), "covered": covered, "missing": len(rules) - covered,
            "issues": issues, "functions": results}


def repository_root():
    if "TEST_SRCDIR" in os.environ:
        return Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
    return Path(__file__).resolve().parents[4]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--backend", choices=("h2peer", "pion"), required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--platform", choices=("macos", "linux", "windows"), required=True)
    parser.add_argument("--process-exit-code", type=int, required=True,
                        help="actual test process status, not the status printed in its log")
    args = parser.parse_args(argv)
    rules = requirements()
    try:
        validate_inventory(rules, (repository_root() / "libs/gizclaw/tests/public_api.inc").read_text())
        with args.log.open(encoding="utf-8") as log:
            result = audit(log, rules, endpoint=args.endpoint, backend=args.backend,
                           profile=args.profile, platform=args.platform,
                           process_exit_code=args.process_exit_code)
    except (OSError, UnicodeError, ValueError):
        print("coverage audit could not read valid inputs", file=sys.stderr)
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
