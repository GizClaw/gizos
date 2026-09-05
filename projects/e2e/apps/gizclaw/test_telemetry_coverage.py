"""Validate one-way transport-acceptance records, not remote persistence."""

import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class TelemetryCoverageTest(unittest.TestCase):
    def test_case_emits_three_api_records_without_claiming_live_acceptance(self):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_telemetry_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run([str(executable), "--emit-success-evidence"],
                             capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0, "Telemetry boundary fixture failed")
        rules = api_coverage.requirements()
        api_coverage.validate_inventory(rules, (root / "libs/gizclaw/tests/public_api.inc").read_text())
        result = api_coverage.audit(run.stdout.splitlines(keepends=True), rules,
                                    endpoint="example.invalid:9821", backend="h2peer",
                                    profile="default", platform="macos", process_exit_code=run.returncode)
        observed = {row["symbol"] for row in result["functions"] if row["status"] == "covered"}
        expected = {rule.symbol for rule in rules if rule.case == "rpc/telemetry"}
        self.assertEqual(len(expected), 3)
        self.assertEqual(observed, expected)
        self.assertFalse(result["valid"])
        self.assertEqual(result["missing"], 178)


if __name__ == "__main__":
    unittest.main()
