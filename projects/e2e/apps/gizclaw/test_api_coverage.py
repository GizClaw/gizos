"""Auditor tests only: synthetic logs never establish real E2E acceptance."""

import unittest

import api_coverage as coverage


IDENTITY = dict(endpoint="edge-bj-01.e2e.gizclaw.com:9821", backend="h2peer",
                profile="default", platform="macos")


def record(**fields):
    return "H2_GIZCLAW_E2E " + " ".join(f"{key}={value}" for key, value in fields.items()) + "\n"


def evidence(symbol, stage, rc=0):
    return record(symbol=symbol, stage=stage, result="PASS" if rc == 0 else "FAIL", rc=rc)


def passing_log(rules):
    lines = []

    def emit_case(case):
        lines.append(record(stage="coverage-begin", case=case))
        for rule in rules:
            if rule.case == case:
                lines.extend(evidence(symbol, "call") for symbol in rule.calls)
                lines.append(evidence(rule.assertion_symbol, rule.assertion_stage))
        if case == "rpc":
            for domain in sorted(coverage.RPC_CASES):
                emit_case("rpc/" + domain)
        lines.append(record(stage="coverage-end", case=case, status="PASS", rc=0, cleanup_rc=0))

    for case in ("connectivity", "rpc", "firmware", "voice", "concurrency", "service"):
        emit_case(case)
    lines.append(record(stage="summary", **IDENTITY, suite="all", selected=6, terminal=6,
                        **{"pass": 6}, fail=0, error=0, blocked=0, cancelled=0,
                        first_failure_case="-", first_failure_rc=0, cleanup_rc=0,
                        retained_resources=0, complete="true", exit_code=0))
    return lines


class CoverageTest(unittest.TestCase):
    def setUp(self):
        self.rules = coverage.requirements()
        self.lines = passing_log(self.rules)

    def audit(self, lines, rules=None, **kwargs):
        return coverage.audit(lines, self.rules if rules is None else rules,
                              **{**IDENTITY, "process_exit_code": 0, **kwargs})

    def test_independent_matrix_matches_all_190_approved_functions(self):
        inventory = (coverage.repository_root() / "libs/gizclaw/tests/public_api.inc").read_text()
        coverage.validate_inventory(self.rules, inventory)
        self.assertEqual(len(self.rules), 190)
        for changed in (self.rules[:-1], self.rules + self.rules[:1]):
            with self.assertRaises(ValueError):
                coverage.validate_inventory(changed, inventory)
        with self.assertRaises(ValueError):
            coverage.validate_inventory(self.rules, inventory.replace("service_init", "service_old"))

    def test_all_functions_need_ordered_calls_and_assertion(self):
        result = self.audit(self.lines)
        self.assertTrue(result["valid"], result["issues"])
        self.assertEqual(result["covered"], 190)
        self.assertEqual(result["missing"], 0)
        for row in result["functions"]:
            self.assertEqual(len(row["call_lines"]), len(row["calls"]))
            self.assertGreater(row["assertion_line"], row["call_lines"][-1])

    def test_each_rule_independently_rejects_missing_call_or_assertion(self):
        for rule in self.rules:
            lines = passing_log([rule])
            for missing in [evidence(symbol, "call") for symbol in rule.calls] + [
                    evidence(rule.assertion_symbol, rule.assertion_stage)]:
                with self.subTest(symbol=rule.symbol, missing=missing):
                    changed = lines.copy()
                    changed.remove(missing)
                    result = self.audit(changed, [rule])
                    self.assertFalse(result["valid"])
                    self.assertEqual(result["missing"], 1)

    def test_header_or_source_symbol_listing_is_not_a_call(self):
        lines = passing_log([])
        lines[1:1] = [f"{rule.symbol}(...);\n" for rule in self.rules]
        result = self.audit(lines)
        self.assertEqual(result["covered"], 0)
        self.assertFalse(result["valid"])

    def test_assertion_before_call_and_wrong_stage_do_not_count(self):
        rule = next(r for r in self.rules if r.symbol == "h2_gizclaw_rpc_profile_get")
        lines = passing_log([rule])
        call = evidence(rule.symbol, "call")
        proof = evidence(rule.assertion_symbol, rule.assertion_stage)
        index = lines.index(call)
        lines[index:index + 2] = [proof, call]
        self.assertEqual(self.audit(lines, [rule])["missing"], 1)
        lines = passing_log([rule])
        lines[lines.index(proof)] = evidence(rule.symbol, "wrong-assert")
        self.assertEqual(self.audit(lines, [rule])["missing"], 1)

    def test_failed_calls_cannot_complete_an_older_partial_chain(self):
        rule = next(r for r in self.rules if r.symbol == "h2_gizclaw_req_create_profile_get")
        lines = passing_log([rule])
        wait = evidence("h2_gizclaw_req_wait", "call")
        index = lines.index(wait)
        lines[index:index + 1] = [evidence("h2_gizclaw_req_wait", "call", -1), wait]
        self.assertEqual(self.audit(lines, [rule])["missing"], 1)

    def test_failed_assertion_is_not_evidence(self):
        rule = self.rules[0]
        lines = passing_log([rule])
        proof = evidence(rule.assertion_symbol, rule.assertion_stage)
        lines[lines.index(proof)] = evidence(rule.assertion_symbol, rule.assertion_stage, -1)
        self.assertEqual(self.audit(lines, [rule])["missing"], 1)

    def test_case_boundaries_reject_missing_duplicate_and_bad_nesting(self):
        begin = record(stage="coverage-begin", case="rpc/profile")
        end = record(stage="coverage-end", case="rpc/profile", status="PASS", rc=0, cleanup_rc=0)
        for removed in (begin, end):
            lines = self.lines.copy()
            lines.remove(removed)
            self.assertFalse(self.audit(lines)["valid"])
        for duplicate in (begin, end):
            lines = self.lines.copy()
            lines.insert(lines.index(duplicate), duplicate)
            self.assertFalse(self.audit(lines)["valid"])
        lines = self.lines.copy()
        lines[lines.index(begin)] = record(stage="coverage-begin", case="voice/profile")
        self.assertFalse(self.audit(lines)["valid"])

    def test_parent_failure_and_cleanup_failure_invalidate_child_coverage(self):
        end = record(stage="coverage-end", case="rpc", status="PASS", rc=0, cleanup_rc=0)
        for status, rc, cleanup in (("FAIL", -1, 0), ("PASS", 0, -1), ("BLOCKED", -1, 0)):
            lines = self.lines.copy()
            lines[lines.index(end)] = record(stage="coverage-end", case="rpc", status=status,
                                            rc=rc, cleanup_rc=cleanup)
            result = self.audit(lines)
            self.assertFalse(result["valid"])
            self.assertTrue(all(row["status"] == "missing" for row in result["functions"]
                                if row["case"].startswith("rpc/")))

    def test_summary_is_mandatory_unique_and_last(self):
        for lines in ([], self.lines[:-1], self.lines + self.lines[-1:], self.lines * 2,
                      self.lines + [evidence(self.rules[0].symbol, "call")]):
            result = self.audit(lines)
            self.assertFalse(result["valid"])
            self.assertTrue(result["issues"])

    def test_printed_success_does_not_override_test_process_failure(self):
        for code in (1, 2, -9, 139):
            result = self.audit(self.lines, process_exit_code=code)
            self.assertFalse(result["valid"])
            self.assertIn("test process did not exit successfully", result["issues"])

    def test_summary_failures_and_identity_cannot_be_overridden_by_coverage(self):
        for key, replacement in dict(fail=1, error=1, blocked=1, cancelled=1, cleanup_rc=-1,
                                     retained_resources=1, complete="false", exit_code=1,
                                     selected=5, terminal=5, suite="connectivity",
                                     platform="linux", backend="pion", profile="other",
                                     endpoint="other:9821", first_failure_rc=-1,
                                     first_failure_case="rpc").items():
            with self.subTest(field=key):
                lines = self.lines.copy()
                fields = coverage._fields(lines[-1][len("H2_GIZCLAW_E2E "):])
                fields[key] = replacement
                lines[-1] = record(**fields)
                self.assertFalse(self.audit(lines)["valid"])

    def test_malformed_records_fail_without_echoing_secrets(self):
        for text in ("symbol=secret stage=coverage-begin", "stage=coverage-begin case=secret case=secret",
                     "stage=coverage-end case=secret", "symbol=h2_gizclaw_req_do stage=call rc=secret result=PASS"):
            lines = self.lines.copy()
            lines.insert(1, "H2_GIZCLAW_E2E " + text + "\n")
            result = self.audit(lines)
            self.assertFalse(result["valid"])
            self.assertNotIn("secret", str(result))

    def test_fingerprint_changes_and_irrelevant_logs_are_not_coverage(self):
        original = self.audit(self.lines)
        changed = self.audit(["unrelated log\n", "H2_GIZCLAW_E2E stage=voice free text\n"] + self.lines)
        self.assertTrue(changed["valid"])
        self.assertNotEqual(original["log_sha256"], changed["log_sha256"])


if __name__ == "__main__":
    unittest.main()
