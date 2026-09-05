"""Check Point case instrumentation with public API doubles, not live E2E."""

import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class PointCoverageTest(unittest.TestCase):
    def test_six_point_functions_have_ordered_calls_and_assertions(self):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_point_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run([str(executable), "--emit-success-evidence"],
                             capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0, "Point boundary fixture failed")
        rules = api_coverage.requirements()
        api_coverage.validate_inventory(rules, (root / "libs/gizclaw/tests/public_api.inc").read_text())
        result = api_coverage.audit(run.stdout.splitlines(keepends=True), rules,
                                    endpoint="example.invalid:9821", backend="h2peer",
                                    profile="default", platform="macos", process_exit_code=run.returncode)
        observed = {row["symbol"] for row in result["functions"] if row["status"] == "covered"}
        expected = {rule.symbol for rule in rules if "_point_" in rule.symbol}
        self.assertEqual(len(expected), 6)
        self.assertEqual(observed, expected)
        # Gameplay's Pet methods and all other cases are still absent here.
        self.assertFalse(result["valid"])
        self.assertEqual(result["missing"], 175)


if __name__ == "__main__":
    unittest.main()
