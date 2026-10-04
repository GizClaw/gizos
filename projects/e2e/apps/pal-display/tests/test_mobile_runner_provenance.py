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

    def test_other_app_catalog_changes_preserve_display_content(self):
        baseline = (qualification.ROOT / "shared_catalog_baseline.txt").read_text(encoding="utf-8")
        current = qualification.Path(qualification.SHARED_CATALOG).read_text(encoding="utf-8")
        self.assertEqual(qualification.display_catalog_content(baseline),
                         qualification.display_catalog_content(current))
        changed = current.replace("## PAL Storage\n", "## Another Storage App\n")
        self.assertEqual(qualification.display_catalog_content(baseline),
                         qualification.display_catalog_content(changed))
        original_read = qualification.Path.read_text
        def read(path, *args, **kwargs):
            if str(path) == qualification.SHARED_CATALOG:
                return changed
            return original_read(path, *args, **kwargs)
        with patch.object(qualification.Path, "read_text", new=read):
            self.verify(self.followup)

    def test_changed_display_section_or_launcher_row_fails(self):
        current = qualification.Path(qualification.SHARED_CATALOG).read_text(encoding="utf-8")
        for changed in [current.replace("固定运行 24 个 mandatory case", "固定运行 1 个 mandatory case"),
                        current.replace("| PAL Display |", "| PAL Display | changed")]:
            with self.subTest(changed=changed):
                original_read = qualification.Path.read_text
                def read(path, *args, **kwargs):
                    if str(path) == qualification.SHARED_CATALOG:
                        return changed
                    return original_read(path, *args, **kwargs)
                with patch.object(qualification.Path, "read_text", new=read):
                    with self.assertRaises(AssertionError):
                        self.verify(self.followup)

    def test_missing_duplicate_display_scopes_fail(self):
        current = qualification.Path(qualification.SHARED_CATALOG).read_text(encoding="utf-8")
        for changed in [current.replace("## PAL Display\n", "## Hidden Display\n"),
                        current + "\n## PAL Display\n", current + "\n| PAL Display | duplicate |\n"]:
            with self.assertRaises(AssertionError):
                qualification.display_catalog_content(changed)

    def test_display_row_must_remain_in_actual_apps_table(self):
        current = qualification.Path(qualification.SHARED_CATALOG).read_text(encoding="utf-8")
        row = qualification.display_catalog_content(current)["launcher_row"]
        removed = current.replace(row, "")
        changed = [removed + "\n" + row,
                   removed.replace("## Atomic\n", row + "\n## Atomic\n"),
                   removed.replace("## Atomic\n", "```text\n" + row + "```\n\n## Atomic\n")]
        for text in changed:
            with self.assertRaises(AssertionError):
                qualification.display_catalog_content(text)

    def test_rewritten_historical_catalog_baseline_fails(self):
        original_read = qualification.Path.read_bytes
        def read(path):
            content = original_read(path)
            return content + b"changed" if path.name == "shared_catalog_baseline.txt" else content
        with patch.object(qualification.Path, "read_bytes", new=read):
            with self.assertRaises(AssertionError):
                self.verify(self.followup)

    def test_catalog_audit_cannot_exempt_provider_source(self):
        path = qualification.ROOT / "shared_catalog_provenance.json"
        audit = json.loads(path.read_text(encoding="utf-8"))
        provider = "libs/pal/providers/sdl3/src/h2_sdl3_display.cpp"
        audit["current_source_sha256"][provider] = "0" * 64
        original_read = qualification.Path.read_text
        def read(source, *args, **kwargs):
            if source == path:
                return json.JSONEncoder().encode(audit)
            return original_read(source, *args, **kwargs)
        with patch.object(qualification.Path, "read_text", new=read):
            with self.assertRaises(AssertionError):
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
