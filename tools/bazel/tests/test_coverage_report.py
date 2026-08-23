from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.bazel import coverage_report


class CoverageReportTest(unittest.TestCase):
    def test_inventories_are_sorted_and_keep_target_identity(self):
        self.assertEqual(
            coverage_report.parse_test_inventory(
                "//z:test\n//a:test\n//skip:test (Incompatible)\n"
            ),
            ["//a:test", "//z:test"],
        )
        targets = coverage_report.parse_target_inventory(
            "//z:lib\tcc_library\t\t"
            "//z:z.c|//z:z.h|//z:tests/z_test.c\n"
            "//a:lib\tcc_library\t\t//a:a.cc\n"
        )
        self.assertEqual(
            [target["label"] for target in targets],
            ["//a:lib", "//z:lib"],
        )
        self.assertEqual(targets[1]["display_name"], "cc_library/lib")
        self.assertEqual(targets[1]["declared_sources"], ["z/z.c", "z/z.h"])

    def test_empty_and_unsupported_target_inventories_fail_closed(self):
        with self.assertRaisesRegex(coverage_report.ReportError, "inventory is empty"):
            coverage_report.parse_target_inventory("")
        with self.assertRaisesRegex(coverage_report.ReportError, "unsupported"):
            coverage_report.parse_target_inventory(
                "//a:firmware\tfirmware_native_component\t\t//a:a.c\n"
            )

    def test_configured_target_json_uses_selected_attributes(self):
        document = {
            "results": [
                {
                    "target": {
                        "rule": {
                            "name": "//a:lib",
                            "ruleClass": "cc_library",
                            "attribute": [
                                {
                                    "name": "srcs",
                                    "stringListValue": ["//a:a.c", "//a:a.h"],
                                },
                                {
                                    "name": "hdrs",
                                    "stringListValue": ["//a:public.hpp"],
                                },
                            ],
                        }
                    }
                },
                {
                    "target": {
                        "rule": {
                            "name": "//windows:lib",
                            "ruleClass": "cc_library",
                            "attribute": [
                                {
                                    "name": "srcs",
                                    "stringListValue": ["//windows:w.c"],
                                },
                            ],
                        }
                    }
                },
            ]
        }
        targets = coverage_report.parse_target_inventory_json(
            json.dumps(document),
            ["//a:lib"],
        )
        self.assertEqual(
            targets[0]["declared_sources"],
            ["a/a.c", "a/a.h", "a/public.hpp"],
        )

    def _write_bep(self, root: Path, events: list[dict[str, object]]) -> Path:
        path = root / "bep.json"
        path.write_text(
            "\n".join(json.dumps(event) for event in events) + "\n",
            encoding="utf-8",
        )
        return path

    def test_bep_reconciles_fresh_passed_tests(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = self._write_bep(
                Path(temporary),
                [
                    {
                        "id": {"testResult": {"label": "//a:test", "run": 1, "attempt": 1, "shard": 0}},
                        "testResult": {"status": "PASSED"},
                    },
                    {
                        "id": {"testSummary": {"label": "//a:test"}},
                        "testSummary": {
                            "overallStatus": "PASSED",
                            "runCount": 1,
                            "attemptCount": 1,
                            "shardCount": 1,
                            "totalRunDurationMillis": 19,
                            "totalNumCached": 0,
                        },
                    },
                    {"id": {"buildFinished": {}}, "finished": {"overallSuccess": True}},
                ],
            )
            report, errors = coverage_report.parse_bep(path, ["//a:test"], 0)
        self.assertEqual(errors, [])
        self.assertEqual(
            report["counts"],
            {
                "expected": 1,
                "passed": 1,
                "failed": 0,
                "incomplete": 0,
                "not_run": 0,
                "cached": 0,
            },
        )
        self.assertEqual(report["tests"][0]["duration_ms"], 19)

    def test_bep_preserves_failed_not_run_and_cached_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = self._write_bep(
                Path(temporary),
                [
                    {
                        "id": {"testSummary": {"label": "//a:failed"}},
                        "testSummary": {"overallStatus": "FAILED", "totalNumCached": 1},
                    }
                ],
            )
            report, errors = coverage_report.parse_bep(
                path,
                ["//a:failed", "//b:not_run"],
                3,
            )
        self.assertEqual(report["counts"]["failed"], 1)
        self.assertEqual(report["counts"]["not_run"], 1)
        self.assertEqual(report["counts"]["cached"], 1)
        self.assertEqual(report["tests"][1]["overall_status"], "NOT_RUN")
        self.assertTrue(any("cached test action" in error for error in errors))

    def test_bep_rejects_duplicate_and_unexpected_summaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            event = {
                "id": {"testSummary": {"label": "//a:test"}},
                "testSummary": {"overallStatus": "PASSED"},
            }
            unexpected = {
                "id": {"testSummary": {"label": "//other:test"}},
                "testSummary": {"overallStatus": "PASSED"},
            }
            path = self._write_bep(Path(temporary), [event, event, unexpected])
            _, errors = coverage_report.parse_bep(path, ["//a:test"], 1)
        self.assertTrue(any("duplicate test summary" in error for error in errors))
        self.assertTrue(any("unexpected first-party" in error for error in errors))

    def test_bep_rejects_malformed_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bep.json"
            path.write_text("{not-json}\n", encoding="utf-8")
            with self.assertRaisesRegex(coverage_report.ReportError, "malformed BEP JSON"):
                coverage_report.parse_bep(path, ["//a:test"], 1)

    def _write_lcov(self, root: Path, contents: str) -> Path:
        path = root / "coverage.dat"
        path.write_text(contents, encoding="utf-8")
        return path

    def test_lcov_metrics_and_absent_branches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "libs/example/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("int example(void) { return 1; }\n", encoding="utf-8")
            lcov = self._write_lcov(
                root,
                f"SF:{source}\n"
                "FNDA:1,covered\nFNDA:0,missed\nFNF:2\nFNH:1\n"
                "DA:1,1\nDA:2,1\nDA:3,1\nDA:4,0\nLF:4\nLH:3\n"
                "end_of_record\n"
                "SF:tools/example.py\nDA:1,1\nLF:1\nLH:1\nend_of_record\n",
            )
            records, filtered = coverage_report.parse_lcov(lcov, root)
        self.assertEqual(records["libs/example/example.c"]["metrics"]["lines"]["percent"], 75.0)
        self.assertEqual(records["libs/example/example.c"]["metrics"]["functions"]["percent"], 50.0)
        self.assertEqual(
            records["libs/example/example.c"]["metrics"]["branches"],
            {"covered": 0, "total": 0, "percent": None},
        )
        self.assertIn("SF:libs/example/example.c", filtered)
        self.assertNotIn("tools/example.py", filtered)

    def test_lcov_rejects_malformed_metrics(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lcov = self._write_lcov(
                root,
                "SF:libs/example/example.c\nFNDA:1,example\nFNF:1\nFNH:2\n"
                "DA:1,1\nLF:1\nLH:1\nend_of_record\n",
            )
            with self.assertRaisesRegex(coverage_report.ReportError, "function summary"):
                coverage_report.parse_lcov(lcov, root)

    def test_target_accounting_distinguishes_all_statuses(self):
        targets = coverage_report.parse_target_inventory(
            "//a:measured\tcc_library\t\t//a:a.c\n"
            "//b:uncovered\tcc_binary\t\t//b:b.cc\n"
            "//c:empty\tcc_library\t\t\n"
        )
        records = {
            "a/a.c": {
                "metrics": {
                    "lines": {"covered": 3, "total": 4, "percent": 75.0},
                    "functions": {"covered": 1, "total": 2, "percent": 50.0},
                    "branches": {"covered": 1, "total": 2, "percent": 50.0},
                },
                "branch_present": True,
            }
        }
        report = coverage_report.build_target_report(targets, records)
        self.assertEqual(
            report["counts"],
            {"eligible": 3, "measured": 1, "uncovered": 1, "not_applicable": 1},
        )
        self.assertEqual(
            [target["status"] for target in report["targets"]],
            ["measured", "uncovered", "not-applicable"],
        )
        self.assertEqual(report["targets"][0]["display_name"], "cc_library/measured")
        self.assertIsNone(report["targets"][1]["metrics"])

    def test_target_accounting_rejects_unknown_lcov_source(self):
        targets = coverage_report.parse_target_inventory(
            "//a:lib\tcc_library\t\t//a:a.c\n"
        )
        with self.assertRaisesRegex(coverage_report.ReportError, "outside eligible targets"):
            coverage_report.build_target_report(
                targets,
                {
                    "unknown.c": {
                        "metrics": {
                            name: {"covered": 0, "total": 0, "percent": None}
                            for name in ("lines", "functions", "branches")
                        }
                    }
                },
            )

    def test_target_accounting_allows_shared_declared_sources(self):
        targets = coverage_report.parse_target_inventory(
            "//a:one\tcc_library\t\t//a:shared.c\n"
            "//a:two\tcc_binary\t\t//a:shared.c\n"
        )
        record = {
            "metrics": {
                name: {"covered": 1, "total": 1, "percent": 100.0}
                for name in ("lines", "functions", "branches")
            }
        }
        report = coverage_report.build_target_report(
            targets,
            {"a/shared.c": record},
        )
        self.assertEqual(
            [target["status"] for target in report["targets"]],
            ["measured", "measured"],
        )
        self.assertEqual(report["measured_overall"]["lines"]["total"], 1)

    def test_summary_labels_measured_scope(self):
        tests = {
            "build_status": "SUCCESS",
            "bazel_exit_code": 0,
            "counts": {
                "expected": 2,
                "passed": 2,
                "failed": 0,
                "incomplete": 0,
                "not_run": 0,
                "cached": 0,
            },
        }
        targets = {
            "counts": {"eligible": 3, "measured": 1, "uncovered": 1, "not_applicable": 1},
            "measured_overall": {
                "lines": {"covered": 3, "total": 4, "percent": 75.0},
                "functions": {"covered": 1, "total": 2, "percent": 50.0},
                "branches": {"covered": 0, "total": 0, "percent": None},
            },
            "targets": [
                {
                    "display_name": "cc_library/example",
                    "package": "libs/example",
                    "status": "measured",
                    "metrics": {
                        "lines": {"covered": 3, "total": 4, "percent": 75.0},
                        "functions": {"covered": 1, "total": 2, "percent": 50.0},
                        "branches": {"covered": 0, "total": 0, "percent": None},
                    },
                }
            ],
        }
        summary = coverage_report.render_summary(tests, targets)
        self.assertIn("Measured-scope coverage (not whole-repository coverage)", summary)
        self.assertIn("Branches | n/a (0 total)", summary)
        self.assertIn("`cc_library/example`", summary)


if __name__ == "__main__":
    unittest.main()
