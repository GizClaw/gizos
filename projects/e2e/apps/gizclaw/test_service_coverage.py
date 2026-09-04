"""Check Service/request evidence wiring without claiming a live E2E run."""

import os
from pathlib import Path
import subprocess
import unittest

import api_coverage


class ServiceCoverageTest(unittest.TestCase):
    def test_voice_and_track_evidence(self):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_voice_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        rules = api_coverage.requirements()
        for mode in range(51):
            with self.subTest(mode=mode):
                run = subprocess.run([str(executable), "--emit-voice-evidence", str(mode)],
                                     capture_output=True, text=True, timeout=30, check=False)
                self.assertEqual(run.returncode, 0, "Voice boundary fixture failed")
                result = api_coverage.audit(run.stdout.splitlines(keepends=True), rules,
                                           endpoint="example.invalid:9821", backend="h2peer",
                                           profile="default", platform="macos", process_exit_code=0)
                observed = {row["symbol"] for row in result["functions"] if row["status"] == "covered"}
                expected = {"h2_gizclaw_service_set_track", "h2_gizclaw_service_unset_track", "h2_gizclaw_service_audio_start", "h2_gizclaw_service_audio_end",
                            "h2_gizclaw_req_create_audio_play"}
                expected.update("h2_gizclaw_conversation_" + name for name in
                                ("create", "cancel", "release"))
                expected.update("h2_gizclaw_pcm_track_" + name for name in
                                ("create", "write", "read", "destroy"))
                if mode not in {0, 16, 50}:
                    expected = set()
                self.assertEqual(observed, expected)
                self.assertFalse(result["valid"])
                self.assertEqual(result["missing"], 190 - len(expected))

    def test_fixture_lifecycle_and_failure_gates(self):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_fixture_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        rules = api_coverage.requirements()
        for mode in range(11):
            with self.subTest(mode=mode):
                run = subprocess.run([str(executable), "--emit-lifecycle-evidence", str(mode)],
                                     capture_output=True, text=True, timeout=30, check=False)
                self.assertEqual(run.returncode, 0, "Lifecycle boundary fixture failed")
                self.assertNotIn("borrowed-token", run.stdout)
                self.assertNotIn("runtime-profile-from-server", run.stdout)
                result = api_coverage.audit(run.stdout.splitlines(keepends=True), rules,
                                           endpoint="example.invalid:9821", backend="h2peer",
                                           profile="default", platform="macos", process_exit_code=0)
                observed = {row["symbol"] for row in result["functions"] if row["status"] == "covered"}
                expected = {"h2_gizclaw_service_" + method for method in
                            ("init", "start", "stop", "deinit")} if mode == 0 else set()
                self.assertEqual(observed, expected)
                self.assertFalse(result["valid"])
                self.assertEqual(result["missing"], 190 - len(expected))

    def test_request_lifecycle_and_poll_records(self):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_request_cases_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run([str(executable), "--emit-success-evidence"],
                             capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0, "Service boundary fixture failed")
        rules = api_coverage.requirements()
        api_coverage.validate_inventory(rules, (root / "libs/gizclaw/tests/public_api.inc").read_text())
        result = api_coverage.audit(run.stdout.splitlines(keepends=True), rules,
                                   endpoint="example.invalid:9821", backend="h2peer",
                                   profile="default", platform="macos", process_exit_code=run.returncode)
        expected = {"h2_gizclaw_req_" + method for method in
                    ("do", "wait", "cancel", "release")}
        expected.add("h2_gizclaw_service_poll")
        observed = {row["symbol"] for row in result["functions"] if row["status"] == "covered"}
        self.assertEqual(observed, expected)
        self.assertFalse(result["valid"])
        self.assertEqual(result["missing"], 185)


if __name__ == "__main__":
    unittest.main()
