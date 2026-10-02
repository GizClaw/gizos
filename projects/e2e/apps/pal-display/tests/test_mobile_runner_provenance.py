"""A host-runner refactor must not weaken historical qualification checks."""
import copy
import json
import unittest
from unittest.mock import patch

import check_qualification as qualification


class MobileRunnerProvenanceTest(unittest.TestCase):
    def setUp(self):
        old = json.loads((qualification.ROOT / "qualification.json").read_text(encoding="utf-8"))
        self.original = {**old["source_sha256"], **old["review_fix_source_sha256"]}
        self.historical = qualification.bk_rgb_buffer_requalification(self.original)
        self.followup = json.loads((qualification.ROOT / "mobile_runner_refactor.json").read_text(encoding="utf-8"))

    def test_new_bk_receipt_cannot_hide_an_unqualified_source_or_failed_case(self):
        followup = json.loads((qualification.ROOT / "bk_rgb_buffer_requalification.json").read_text())
        receipt = json.loads(qualification.Path(followup["board_evidence"]).read_text())
        build = json.loads(qualification.Path(followup["build_evidence"]).read_text())
        qualification.verify_bk_rgb_buffers(followup, self.original, receipt, build)
        wrong = copy.deepcopy(followup)
        wrong["current_source_sha256"]["libs/pal/providers/sdl3/src/h2_sdl3_display.cpp"] = "0" * 64
        with self.assertRaises(AssertionError):
            qualification.verify_bk_rgb_buffers(wrong, self.original, receipt, build)
        wrong = copy.deepcopy(receipt)
        wrong["independent_normal_reboot"]["cases"][0]["status"] = "FAIL"
        with self.assertRaises(AssertionError):
            qualification.verify_bk_rgb_buffers(followup, self.original, wrong, build)
        wrong = copy.deepcopy(receipt)
        wrong["independent_normal_reboot"]["log_sha256"] = wrong["install_boot"]["log_sha256"]
        with self.assertRaises(AssertionError):
            qualification.verify_bk_rgb_buffers(followup, self.original, wrong, build)
        wrong = copy.deepcopy(receipt)
        wrong["physical_observation"]["stable_image_observed"] = False
        with self.assertRaises(AssertionError):
            qualification.verify_bk_rgb_buffers(followup, self.original, wrong, build)

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
