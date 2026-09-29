"""Metadata call wiring; these local doubles do not establish live qualification."""
import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class MetadataCoverageTest(unittest.TestCase):
    def test_actual_case_evidence_covers_nine_metadata_apis(self):
        root = api_coverage.repository_root()
        exe = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_metadata_test"
        if os.name == "nt":
            exe = Path(str(exe) + ".exe")
        run = subprocess.run([str(exe), "--emit-success-evidence"],
                             capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0)
        result = api_coverage.audit(run.stdout.splitlines(keepends=True),
                                   api_coverage.requirements(),
                                   endpoint="example.invalid:9821", backend="h2peer",
                                   profile="profile", platform="macos", process_exit_code=0)
        expected = {"h2_gizclaw_" + prefix + "_" + method
                    for prefix in ("req_create", "resp_parse", "rpc")
                    for method in ("app_config_list", "app_config_get", "public_profile_get")}
        self.assertEqual({x["symbol"] for x in result["functions"]
                          if x["status"] == "covered"}, expected)
        self.assertFalse(result["valid"])
        self.assertEqual(result["missing"], 226 - 9)
        self.assertNotIn("value=value", run.stdout)

    def test_social_ping_requires_receiver_evidence(self):
        root = api_coverage.repository_root()
        exe = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_social_ping_test"
        if os.name == "nt":
            exe = Path(str(exe) + ".exe")
        run = subprocess.run([str(exe), "--emit-success-evidence"],
                             capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0)
        result = api_coverage.audit(run.stdout.splitlines(keepends=True),
                                   api_coverage.requirements(),
                                   endpoint="example.invalid:9821", backend="h2peer",
                                   profile="profile", platform="macos", process_exit_code=0)
        expected = {"h2_gizclaw_" + prefix + "_" + method
                    for prefix in ("req_create", "resp_parse", "rpc")
                    for method in ("friend_ping", "friend_group_ping")}
        self.assertEqual({x["symbol"] for x in result["functions"]
                          if x["status"] == "covered"}, expected)
        self.assertFalse(result["valid"])
        self.assertEqual(result["missing"], 226 - 6)


if __name__ == "__main__":
    unittest.main()
