"""A host-runner refactor must not weaken historical qualification checks."""
import copy
import hashlib
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

    def test_ipv6_maintenance_does_not_admit_unowned_or_physical_changes(self):
        path = qualification.ROOT / "shared_ipv6_maintenance.json"
        record = json.loads(path.read_text(encoding="utf-8"))
        original_read = qualification.Path.read_text
        mutations = [
            lambda value: value.update(new_physical_run_claimed=True),
            lambda value: value.update(historical_baseline_sha256="0" * 64),
            lambda value: value["guide_changes"].append({"owner": "display", "before_utf8": "", "after_utf8": "unowned"}),
            lambda value: value["guide_changes"].append(copy.deepcopy(value["guide_changes"][0])),
            lambda value: value["current_audit_sha256"].update({"libs/pal/providers/sdl3/src/h2_sdl3_display.cpp": "0" * 64}),
        ]
        for mutate in mutations:
            bad = copy.deepcopy(record)
            mutate(bad)
            def read(source, *args, **kwargs):
                return json.JSONEncoder().encode(bad) if source == path else original_read(source, *args, **kwargs)
            with patch.object(qualification.Path, "read_text", new=read):
                with self.assertRaises(AssertionError):
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
            if path == qualification.Path(qualification.SHARED_CATALOG):
                return changed
            return original_read(path, *args, **kwargs)
        with patch.object(qualification.Path, "read_text", new=read):
            self.verify(self.followup)

    def test_bk_network_change_rejects_checksum_consistent_unowned_hunks(self):
        cfg = qualification.Path("boards/bk7258_v3_202405/bk7258/ap.defaults")
        path = qualification.ROOT / "shared_ipv6_maintenance.json"
        record = json.loads(path.read_text())
        before = b"# CONFIG_IPV6 is not set\n"
        enabled = b"CONFIG_IPV6=y\n"
        baseline = cfg.read_bytes()
        if enabled in baseline:
            baseline = baseline.replace(enabled, before, 1)
        self.assertEqual(hashlib.sha256(baseline).hexdigest(),
                         self.historical[str(cfg)])
        for after in (b"CONFIG_IPV6=n\n",
                      b"CONFIG_IPV6=y\nCONFIG_LWIP_IPV6_NUM_ADDRESSES=1\n"):
            content = baseline.replace(before, after, 1)
            bad = copy.deepcopy(record)
            bad["network_config_changes"] = {str(cfg): {
                "previous_sha256": self.historical[str(cfg)],
                "current_sha256": hashlib.sha256(content).hexdigest(),
                "before_utf8": before.decode(), "after_utf8": after.decode(),
            }}
            original_text = qualification.Path.read_text
            original_bytes = qualification.Path.read_bytes
            def read_text(source, *args, **kwargs):
                return (json.JSONEncoder().encode(bad) if source == path
                        else original_text(source, *args, **kwargs))
            def read_bytes(source):
                return content if source == cfg else original_bytes(source)
            with patch.object(qualification.Path, "read_text", new=read_text), \
                    patch.object(qualification.Path, "read_bytes", new=read_bytes):
                with self.assertRaises(AssertionError):
                    self.verify(self.followup)

    def test_changed_display_section_or_launcher_row_fails(self):
        current = qualification.Path(qualification.SHARED_CATALOG).read_text(encoding="utf-8")
        for variant, changed in [
                ("section", current.replace("固定运行 24 个 mandatory case", "固定运行 1 个 mandatory case")),
                ("launcher_row", current.replace("| PAL Display |", "| PAL Display | changed"))]:
            with self.subTest(variant=variant):
                original_read = qualification.Path.read_text
                def read(path, *args, **kwargs):
                    if path == qualification.Path(qualification.SHARED_CATALOG):
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

    def test_shared_pal_guide_only_admits_exact_pref_extension(self):
        audit = json.loads((qualification.ROOT / "shared_catalog_provenance.json").read_text(encoding="utf-8"))
        extension = audit["pal_guide_extension"]
        path = qualification.Path(qualification.SHARED_PAL_GUIDE)
        current = qualification.shared_pal_before_ipv6(path.read_bytes())
        addition = json.loads((qualification.ROOT / "shared_pal_pref_addition.json").read_text(encoding="utf-8"))["addition_utf8"].encode("utf-8")
        offset = extension["insertion_offset"]
        baseline = current if qualification.hashlib.sha256(current).hexdigest() == extension["source_sha256"] else (
            current[:offset] + current[offset + len(addition):])
        extended = baseline[:offset] + addition + baseline[offset:]
        previous = {qualification.SHARED_PAL_GUIDE: extension["source_sha256"]}
        original_read = qualification.Path.read_bytes
        for content, accepted in [(extended, True), (baseline, True),
                                  (extended + b"\nunknown PAL policy\n", False),
                                  (extended.replace(b"Display", b"ChangedDisplay", 1), False),
                                  (baseline + addition, False),
                                  (extended.replace(b"Tail v1 manifest", b"Tail v2 manifest", 1), False)]:
            def read(source):
                return content if source == path else original_read(source)
            with patch.object(qualification.Path, "read_bytes", new=read):
                if accepted:
                    qualification.shared_pal_guide(previous, extension)
                else:
                    with self.assertRaises(AssertionError):
                        qualification.shared_pal_guide(previous, extension)

    def test_changed_pref_addition_fixture_and_sidecar_cannot_rebind_source(self):
        audit = json.loads((qualification.ROOT / "shared_catalog_provenance.json").read_text(encoding="utf-8"))
        extension = audit["pal_guide_extension"]
        fixture_path = qualification.ROOT / "shared_pal_pref_addition.json"
        fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
        addition = fixture["addition_utf8"].encode("utf-8")
        path = qualification.Path(qualification.SHARED_PAL_GUIDE)
        current = qualification.shared_pal_before_ipv6(path.read_bytes())
        offset = extension["insertion_offset"]
        baseline = current if qualification.hashlib.sha256(current).hexdigest() == extension["source_sha256"] else (
            current[:offset] + current[offset + len(addition):])
        fixture["addition_utf8"] = fixture["addition_utf8"].replace("legacy KVDB", "changed legacy KVDB", 1)
        changed = fixture["addition_utf8"].encode("utf-8")
        self.assertNotEqual(addition, changed)
        extension["addition_sha256"] = qualification.hashlib.sha256(changed).hexdigest()
        original_text = qualification.Path.read_text
        original_bytes = qualification.Path.read_bytes
        def read_text(source, *args, **kwargs):
            return json.JSONEncoder().encode(fixture) if source == fixture_path else original_text(source, *args, **kwargs)
        def read_bytes(source):
            return baseline[:offset] + changed + baseline[offset:] if source == path else original_bytes(source)
        with patch.object(qualification.Path, "read_text", new=read_text), patch.object(
                qualification.Path, "read_bytes", new=read_bytes):
            with self.assertRaises(AssertionError):
                qualification.shared_pal_guide({qualification.SHARED_PAL_GUIDE: extension["source_sha256"]}, extension)

    def test_moved_pref_addition_and_sidecar_cannot_rebind_insertion(self):
        audit = json.loads((qualification.ROOT / "shared_catalog_provenance.json").read_text(encoding="utf-8"))
        extension = audit["pal_guide_extension"]
        addition = json.loads((qualification.ROOT / "shared_pal_pref_addition.json").read_text(encoding="utf-8"))["addition_utf8"].encode("utf-8")
        path = qualification.Path(qualification.SHARED_PAL_GUIDE)
        current = path.read_bytes()
        offset = extension["insertion_offset"]
        baseline = current if qualification.hashlib.sha256(current).hexdigest() == extension["source_sha256"] else (
            current[:offset] + current[offset + len(addition):])
        extension["insertion_offset"] = 0
        original_read = qualification.Path.read_bytes
        def read(source):
            return addition + baseline if source == path else original_read(source)
        with patch.object(qualification.Path, "read_bytes", new=read):
            with self.assertRaises(AssertionError):
                qualification.shared_pal_guide({qualification.SHARED_PAL_GUIDE: extension["source_sha256"]}, extension)

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
