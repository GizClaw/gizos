"""Audit production Pet case evidence using local public API doubles."""

import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class PetCoverageTest(unittest.TestCase):
    def audit_case(self, *args):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_pet_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run([str(executable), *args], capture_output=True,
                             text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        rules = api_coverage.requirements()
        api_coverage.validate_inventory(
            rules, (root / "libs/gizclaw/tests/public_api.inc").read_text())
        result = api_coverage.audit(
            run.stdout.splitlines(keepends=True), rules,
            endpoint="example.invalid:9821", backend="h2peer", profile="default",
            platform="macos", process_exit_code=run.returncode)
        # No live identity or full suite: local coverage is not acceptance.
        self.assertFalse(result["valid"])
        return {row["symbol"] for row in result["functions"]
                if row["status"] == "covered"}

    def test_twenty_one_functions(self):
        expected = {rule.symbol for rule in api_coverage.requirements()
                    if "_pet_" in rule.symbol}
        self.assertEqual(len(expected), 21)
        self.assertEqual(self.audit_case("--emit-success-evidence"), expected)
        for variant in (1, 4, 8):
            self.assertEqual(self.audit_case("--emit-variant-evidence", str(variant)), expected)
        for variant in (2, 3, 5, 6, 7, 9, 10):
            self.assertEqual(self.audit_case("--emit-variant-evidence", str(variant)), set())

    def test_failure_invalidates_partial_evidence(self):
        for failure, budget in ([(i, 0) for i in range(1, 47)] +
                                [(0, i) for i in range(1, 23)]):
            with self.subTest(failure=failure, budget=budget):
                self.assertEqual(self.audit_case("--emit-failure-evidence",
                                                 str(failure), str(budget)), set())


if __name__ == "__main__":
    unittest.main()
