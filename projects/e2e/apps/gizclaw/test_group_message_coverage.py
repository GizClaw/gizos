"""Audit actual message-case call logs; boundary doubles are not live E2E."""

import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class GroupMessageCoverageTest(unittest.TestCase):
    def audit(self, fail=0, budget=0, fault=0, response=0, pagination=0):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_group_message_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run(
            [str(executable), *map(str, (fail, budget, fault, response, pagination))],
            capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        rules = api_coverage.requirements()
        api_coverage.validate_inventory(
            rules, (root / "libs/gizclaw/tests/public_api.inc").read_text())
        result = api_coverage.audit(
            run.stdout.splitlines(keepends=True), rules,
            endpoint="example.invalid:9821", backend="h2peer", profile="default",
            platform="macos", process_exit_code=run.returncode)
        self.assertFalse(result["valid"])
        return {row["symbol"] for row in result["functions"]
                if row["status"] == "covered"}

    def test_six_functions(self):
        expected = {rule.symbol for rule in api_coverage.requirements()
                    if rule.symbol.endswith(("friend_group_message_list",
                                             "friend_group_message_get"))}
        self.assertEqual(len(expected), 6)
        for pagination in (0, 1, 6, 8, 9, 10):
            self.assertEqual(self.audit(pagination=pagination), expected)

    def test_failure_invalidates_partial_success(self):
        for stage in range(1, 11):
            self.assertEqual(self.audit(fail=stage), set())
        for budget in range(1, 5):
            self.assertEqual(self.audit(budget=budget), set())
        for response in range(1, 5):
            for fault in range(1, 27 if response % 2 else 19):
                with self.subTest(response=response, fault=fault):
                    self.assertEqual(self.audit(fault=fault, response=response), set())
        for pagination in (2, 3, 4, 5, 7):
            self.assertEqual(self.audit(pagination=pagination), set())


if __name__ == "__main__":
    unittest.main()
