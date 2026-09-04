"""Audit the real Friend case's boundary log, not a live server run."""

import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class FriendCoverageTest(unittest.TestCase):
    def test_all_twenty_one_functions_emit_calls_and_assertions(self):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_friend_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run([str(executable), "--emit-success-evidence"],
                             capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0, "Friend boundary fixture failed")
        self.assertNotIn("local-invite-", run.stdout)
        rules = api_coverage.requirements()
        api_coverage.validate_inventory(rules, (root / "libs/gizclaw/tests/public_api.inc").read_text())
        result = api_coverage.audit(run.stdout.splitlines(keepends=True), rules,
                                   endpoint="example.invalid:9821", backend="h2peer",
                                   profile="default", platform="macos", process_exit_code=run.returncode)
        expected = {rule.symbol for rule in rules if rule.case == "rpc/friend"}
        observed = {row["symbol"] for row in result["functions"] if row["status"] == "covered"}
        self.assertEqual(len(expected), 21)
        self.assertEqual(observed, expected)
        self.assertFalse(result["valid"])
        self.assertEqual(result["missing"], 160)


if __name__ == "__main__":
    unittest.main()
