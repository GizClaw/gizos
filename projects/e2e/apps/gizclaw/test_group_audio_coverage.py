"""Check group audio evidence without claiming local doubles are live E2E."""

import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class GroupAudioCoverageTest(unittest.TestCase):
    def audit(self, fail=0, budget=0, fault=0, api=0):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_group_audio_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run([str(executable), *map(str, (fail, budget, fault, api))],
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

    def test_three_functions(self):
        expected = {rule.symbol for rule in api_coverage.requirements()
                    if rule.symbol.endswith("friend_group_message_audio_download")}
        self.assertEqual(len(expected), 3)
        self.assertEqual(self.audit(), expected)

    def test_failure_invalidates_partial_success(self):
        for stage in range(1, 6):
            with self.subTest(stage=stage):
                self.assertEqual(self.audit(fail=stage), set())
        for budget in range(1, 3):
            self.assertEqual(self.audit(budget=budget), set())
        for api in range(2):
            for fault in range(1, 15):
                with self.subTest(api=api, fault=fault):
                    self.assertEqual(self.audit(fault=fault, api=api), set())


if __name__ == "__main__":
    unittest.main()
