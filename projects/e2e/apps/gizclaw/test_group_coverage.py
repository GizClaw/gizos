"""Verify management API evidence without substituting doubles for live E2E."""
import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class GroupCoverageTest(unittest.TestCase):
    def audit(self, fail=0, budget=0, reply=0, fault=0, mutation=0, pages=0):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_group_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run(
            [str(executable), *map(str, (fail, budget, reply, fault, mutation, pages))],
            capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertNotIn("secret-token", run.stdout)
        rules = api_coverage.requirements()
        api_coverage.validate_inventory(
            rules, (root / "libs/gizclaw/tests/public_api.inc").read_text())
        result = api_coverage.audit(
            run.stdout.splitlines(keepends=True), rules, endpoint="example.invalid:9821",
            backend="h2peer", profile="default", platform="macos",
            process_exit_code=run.returncode)
        self.assertFalse(result["valid"])
        return {row["symbol"] for row in result["functions"]
                if row["status"] == "covered"}

    def test_thirty_six_functions(self):
        expected = {rule.symbol for rule in api_coverage.requirements()
                    if "_friend_group_" in rule.symbol and "_message_" not in rule.symbol}
        self.assertEqual(len(expected), 36)
        self.assertEqual(self.audit(), expected)
        self.assertEqual(self.audit(pages=1), expected)

    def test_failures_discard_partial_coverage(self):
        for stage in range(1, 85):
            with self.subTest(stage=stage):
                self.assertEqual(self.audit(fail=stage), set())
        for budget in range(1, 37):
            self.assertEqual(self.audit(budget=budget), set())
        for mutation in range(1, 18):
            self.assertEqual(self.audit(mutation=mutation), set())
        for pages in range(2, 6):
            self.assertEqual(self.audit(pages=pages), set())


if __name__ == "__main__":
    unittest.main()
