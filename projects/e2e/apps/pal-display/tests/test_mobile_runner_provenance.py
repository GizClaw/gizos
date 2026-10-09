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

    def guide_format_record(self):
        return json.loads((qualification.ROOT / "bk_display_guide_format_provenance.json").read_text(encoding="utf-8"))

    def verify_guide_format(self, record, guide=None):
        fixture_path = qualification.ROOT / "bk_display_guide_format_provenance.json"
        guide_path = qualification.Path(qualification.BK_DISPLAY_GUIDE)
        original_text = qualification.Path.read_text
        original_bytes = qualification.Path.read_bytes
        def read_text(source, *args, **kwargs):
            return json.JSONEncoder().encode(record) if source == fixture_path else original_text(source, *args, **kwargs)
        def read_bytes(source):
            return guide if guide is not None and source == guide_path else original_bytes(source)
        with patch.object(qualification.Path, "read_text", new=read_text), patch.object(
                qualification.Path, "read_bytes", new=read_bytes):
            return qualification.bk_display_guide_format_source(
                qualification.BK_DISPLAY_GUIDE_FORMAT_BASELINE_SHA256)

    def test_exact_bk_guide_format_projects_only_the_doc_hash(self):
        record = self.guide_format_record()
        expected = record["current_source_sha256"][qualification.BK_DISPLAY_GUIDE]
        self.assertEqual(self.verify_guide_format(record), expected)
        self.assertEqual(self.historical[qualification.BK_DISPLAY_GUIDE], expected)
        followup = json.loads((qualification.ROOT / "bk_rgb_buffer_requalification.json").read_text(encoding="utf-8"))
        self.assertNotEqual(followup["current_source_sha256"][qualification.BK_DISPLAY_GUIDE], expected)
        for path, sha in followup["current_source_sha256"].items():
            if path != qualification.BK_DISPLAY_GUIDE:
                self.assertEqual(self.historical[path], sha)

    def test_bk_guide_format_rejects_semantic_structure_and_other_format_changes(self):
        current = qualification.Path(qualification.BK_DISPLAY_GUIDE).read_bytes()
        variants = [
            current.replace(b"canonical shadow", b"mutable shadow", 1),
            current.replace("## 预期表现".encode(), "## Another section".encode(), 1),
            current.replace(b"H2_BK7258_DISPLAY_DIAGNOSTICS=1", b"H2_BK7258_DISPLAY_DIAGNOSTICS=0", 1),
            current.replace("H050IWV 800×480 RGB".encode(), "H050IWV 800×480 QSPI".encode(), 1),
            current.replace("dirty 矩形".encode(), "dirty矩形".encode(), 1),
            current + b"\n```c\nchanged_driver();\n```\n",
        ]
        for guide in variants:
            self.assertNotEqual(current, guide)
            record = self.guide_format_record()
            record["current_source_sha256"][qualification.BK_DISPLAY_GUIDE] = qualification.hashlib.sha256(guide).hexdigest()
            with self.assertRaises(AssertionError):
                self.verify_guide_format(record, guide)

    def test_bk_guide_format_baseline_and_hash_cannot_be_rebound(self):
        for rewrite_hash in (False, True):
            record = self.guide_format_record()
            record["baseline_utf8"] = record["baseline_utf8"].replace("canonical shadow", "mutable shadow", 1)
            if rewrite_hash:
                record["baseline_sha256"] = qualification.hashlib.sha256(record["baseline_utf8"].encode()).hexdigest()
            guide = qualification.Path(qualification.BK_DISPLAY_GUIDE).read_bytes().replace(b"canonical shadow", b"mutable shadow", 1)
            record["current_source_sha256"][qualification.BK_DISPLAY_GUIDE] = qualification.hashlib.sha256(guide).hexdigest()
            with self.assertRaises(AssertionError):
                self.verify_guide_format(record, guide)

    def test_bk_guide_format_scope_cannot_expand_or_claim_physical_execution(self):
        original = self.guide_format_record()
        provider = "boards/bk7258_v3_202405/bk7258/ap/src/h2_bk7258_board_display.c"
        provider_sha = qualification.hashlib.sha256(qualification.Path(provider).read_bytes()).hexdigest()
        variants = []
        record = copy.deepcopy(original)
        record["current_source_sha256"][provider] = provider_sha
        variants.append(record)
        record = copy.deepcopy(original)
        record["source_path"] = provider
        variants.append(record)
        record = copy.deepcopy(original)
        record["paragraph_prefixes_utf8"].append("H050IWV 800×480 RGB")
        variants.append(record)
        record = copy.deepcopy(original)
        record["new_physical_run_claimed"] = True
        variants.append(record)
        for record in variants:
            with self.assertRaises(AssertionError):
                self.verify_guide_format(record)

    def test_bk_guide_format_cannot_hide_changed_framebuffer_bytes(self):
        provider = qualification.Path("boards/bk7258_v3_202405/bk7258/ap/src/h2_bk7258_board_display.c")
        changed = provider.read_bytes() + b"\nchanged_framebuffer_source\n"
        original_read = qualification.Path.read_bytes
        def read(source):
            return changed if source == provider else original_read(source)
        with patch.object(qualification.Path, "read_bytes", new=read):
            with self.assertRaises(AssertionError):
                qualification.bk_rgb_buffer_requalification(self.original)

    def test_bk_guide_format_cannot_retag_historical_source(self):
        followup = json.loads((qualification.ROOT / "bk_rgb_buffer_requalification.json").read_text(encoding="utf-8"))
        receipt = json.loads(qualification.Path(followup["board_evidence"]).read_text(encoding="utf-8"))
        build = json.loads(qualification.Path(followup["build_evidence"]).read_text(encoding="utf-8"))
        followup["current_source_sha256"][qualification.BK_DISPLAY_GUIDE] = self.guide_format_record()["current_source_sha256"][qualification.BK_DISPLAY_GUIDE]
        with self.assertRaises(AssertionError):
            qualification.verify_bk_rgb_buffers(followup, self.original, receipt, build)

    def test_new_bk_receipt_cannot_hide_an_unqualified_source_or_failed_case(self):
        followup = json.loads((qualification.ROOT / "bk_rgb_buffer_requalification.json").read_text(encoding="utf-8"))
        receipt = json.loads(qualification.Path(followup["board_evidence"]).read_text(encoding="utf-8"))
        build = json.loads(qualification.Path(followup["build_evidence"]).read_text(encoding="utf-8"))
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

    def test_modem_guide_changes_preserve_display_qualification(self):
        path = qualification.Path(qualification.SHARED_PAL_GUIDE)
        current = path.read_bytes()
        start, end = qualification.modem_guide_section(current)
        changed = current[:start] + current[start:end].replace(
            "两个调用".encode("utf-8"), "Modem 两个调用".encode("utf-8"), 1) + current[end:]
        self.assertNotEqual(current, changed)
        original_read = qualification.Path.read_bytes
        def read(source):
            return changed if source == path else original_read(source)
        with patch.object(qualification.Path, "read_bytes", new=read):
            self.verify(self.followup)

    def test_modem_scope_cannot_exempt_touch_or_general_pal_changes(self):
        path = qualification.Path(qualification.SHARED_PAL_GUIDE)
        current = path.read_bytes()
        start, end = qualification.modem_guide_section(current)
        variants = [
            current.replace("Touch PAL 不识别".encode("utf-8"), b"Changed Touch policy", 1),
            current[:end] + b"### Display\nInjected policy\n\n" + current[end:],
            current[:end] + b"###\tDisplay\nInjected policy\n\n" + current[end:],
            current[:end] + b"   ###  Display\nInjected policy\n\n" + current[end:],
            current[:end] + b"###\nInjected empty heading\n\n" + current[end:],
            current[:end] + b"Display\n=======\nInjected policy\n\n" + current[end:],
            current + "\n### Modem 通话扬声器音量\n".encode("utf-8"),
            current[:start] + current[end:] + current[start:end],
        ]
        original_read = qualification.Path.read_bytes
        for changed in variants:
            self.assertNotEqual(current, changed)
            def read(source):
                return changed if source == path else original_read(source)
            with patch.object(qualification.Path, "read_bytes", new=read):
                with self.assertRaises(AssertionError):
                    self.verify(self.followup)

    def test_modem_fixture_and_sidecar_cannot_rebind_historical_bytes(self):
        fixture_path = qualification.ROOT / "shared_pal_modem_baseline.json"
        fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
        fixture["section_utf8"] += "changed baseline\n"
        audit_path = qualification.ROOT / "shared_catalog_provenance.json"
        audit = json.loads(audit_path.read_text(encoding="utf-8"))
        audit["pal_modem_scope"]["baseline_sha256"] = qualification.hashlib.sha256(
            fixture["section_utf8"].encode("utf-8")).hexdigest()
        original_read = qualification.Path.read_text
        def read(source, *args, **kwargs):
            if source == fixture_path:
                return json.JSONEncoder().encode(fixture)
            if source == audit_path:
                return json.JSONEncoder().encode(audit)
            return original_read(source, *args, **kwargs)
        with patch.object(qualification.Path, "read_text", new=read):
            with self.assertRaises(AssertionError):
                self.verify(self.followup)

    def test_shared_pal_guide_only_admits_exact_pref_extension(self):
        audit = json.loads((qualification.ROOT / "shared_catalog_provenance.json").read_text(encoding="utf-8"))
        extension = audit["pal_guide_extension"]
        path = qualification.Path(qualification.SHARED_PAL_GUIDE)
        current = qualification.historical_modem_guide(path.read_bytes())
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
        current = qualification.historical_modem_guide(path.read_bytes())
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
        current = qualification.historical_modem_guide(path.read_bytes())
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
