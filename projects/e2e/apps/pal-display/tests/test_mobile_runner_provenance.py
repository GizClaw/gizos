"""A host-runner refactor must not weaken historical qualification checks."""
import copy
import json
import unittest
from unittest.mock import patch

import check_qualification as qualification


class MobileRunnerProvenanceTest(unittest.TestCase):
    def setUp(self):
        old = json.loads((qualification.ROOT / "qualification.json").read_text(encoding="utf-8"))
        self.historical = {**old["source_sha256"], **old["review_fix_source_sha256"]}
        self.followup = json.loads((qualification.ROOT / "mobile_runner_refactor.json").read_text(encoding="utf-8"))

    def verify(self, followup):
        with patch.object(qualification.json, "loads", return_value=followup):
            qualification.runner_refactor(self.historical)

    def test_separate_mobile_runs_preserve_historical_qualification(self):
        self.verify(self.followup)

    def test_native_source_cannot_be_exempted_as_runner_source(self):
        changed = copy.deepcopy(self.followup)
        changed["current_source_sha256"]["libs/pal/providers/sdl3/src/h2_sdl3_display.cpp"] = "0" * 64
        with self.assertRaises(AssertionError):
            self.verify(changed)

    def test_audit_source_exemption_cannot_hide_a_changed_provider(self):
        changed = copy.deepcopy(self.followup)
        changed["audit_source_sha256"]["libs/pal/providers/sdl3/src/h2_sdl3_display.cpp"] = "0" * 64
        with self.assertRaises(AssertionError):
            self.verify(changed)
        changed = copy.deepcopy(self.followup)
        changed["audit_source_sha256"]["Makefile"] = "0" * 64
        with self.assertRaises(AssertionError):
            self.verify(changed)

    def test_changed_native_source_still_fails(self):
        self.historical["libs/pal/providers/sdl3/src/h2_sdl3_display.cpp"] = "0" * 64
        with self.assertRaises(AssertionError):
            self.verify(self.followup)

    def test_failed_mobile_case_or_false_physical_claim_fails(self):
        changed = copy.deepcopy(self.followup)
        changed["mobile_runs"]["android"]["qualified"]["cases"][0]["status"] = "FAIL"
        with self.assertRaises(AssertionError):
            self.verify(changed)
        changed = copy.deepcopy(self.followup)
        changed["new_physical_run_claimed"] = True
        with self.assertRaises(AssertionError):
            self.verify(changed)

    def test_unexecuted_bazel_declaration_fails(self):
        changed = copy.deepcopy(self.followup)
        changed["mobile_runs"]["ios"]["environment"]["suite_sha256"] = "0" * 64
        with self.assertRaises(AssertionError):
            self.verify(changed)

    def test_unexecuted_python_or_unbound_historical_report_fails(self):
        for key in ("executed_runner_sha256", "removed_source_sha256"):
            changed = copy.deepcopy(self.followup)
            changed[key][next(iter(changed[key]))] = "0" * 64
            with self.assertRaises(AssertionError):
                self.verify(changed)
        changed = copy.deepcopy(self.followup)
        changed["historical_qualification_sha256"] = "0" * 64
        with self.assertRaises(AssertionError):
            self.verify(changed)


if __name__ == "__main__":
    unittest.main()
