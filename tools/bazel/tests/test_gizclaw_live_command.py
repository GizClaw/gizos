"""Exercise the real workflow shell, Make and wrappers without running Bazel."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).absolute().parents[3]
PREFIX = "//projects/e2e/targets/cc_test/gizclaw:gizclaw_"


class LiveCommandTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gizclaw-command-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.capture = self.directory / "capture.jsonl"
        fake = self.directory / "bazel"
        fake.write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "with open(os.environ['COMMAND_CAPTURE'], 'a') as output:\n"
            "    output.write(json.dumps({'argv': sys.argv[1:], "
            "'suite': os.environ.get('H2_GIZCLAW_E2E_SUITE')}) + '\\n')\n"
            "raise SystemExit(int(os.environ.get('COMMAND_FAIL_RC', '0')) "
            "if os.environ.get('COMMAND_FAIL_LABEL') == sys.argv[-1] else 0)\n"
        )
        fake.chmod(0o700)
        # Never inherit a real token, endpoint or user's remote cache settings.
        self.env = {
            "PATH": os.environ["PATH"],
            "RUNNER_TEMP": str(self.directory),
            "BAZEL_BIN": str(fake),
            "H2_BAZEL_CONFIG": "macos_arm64",
            "COMMAND_CAPTURE": str(self.capture),
            "H2_GIZCLAW_E2E_ENDPOINT": "edge-bj-01.e2e.gizclaw.com:9821",
            "H2_GIZCLAW_E2E_SUITE": "all",
            "E2E_SCOPE": "gizclaw",
            "E2E_BACKEND": "h2peer",
        }

    def calls(self):
        if not self.capture.exists():
            return []
        return [json.loads(line) for line in self.capture.read_text().splitlines()]

    def run_command(self, argv):
        return subprocess.run(
            argv, cwd=ROOT, env=self.env, text=True,
            capture_output=True, timeout=20, check=False,
        )

    def workflow(self):
        text = (ROOT / ".github/workflows/e2e.yml").read_text()
        step = text.split("      - name: Run selected live E2E\n", 1)[1]
        self.assertIn("H2_GIZCLAW_E2E_ENDPOINT: ${{ inputs.endpoint }}", step)
        self.assertIn("E2E_BACKEND: ${{ inputs.backend }}", step)
        self.assertNotIn("inputs.entry", text)
        self.assertNotIn("H2_GIZCLAW_E2E_ENTRY", text)
        body = step.split("        run: |\n", 1)[1]
        lines = []
        for line in body.splitlines():
            if line and not line.startswith("          "):
                break
            lines.append(line[10:])
        script = "\n".join(lines)
        self.assertNotIn("${{", script, "inputs must pass through quoted env")
        return self.run_command(["bash", "-c", script])

    def assert_live_call(self, call, backend):
        args = call["argv"]
        self.assertEqual(args[0], "test")
        self.assertIn("--config=macos_arm64", args)
        self.assertIn("--cache_test_results=no", args)
        self.assertEqual(args[-1], PREFIX + backend + "_live_test")
        self.assertEqual(
            [arg for arg in args if arg.startswith("--test_arg=")],
            ["--test_arg=--endpoint=" + self.env["H2_GIZCLAW_E2E_ENDPOINT"]],
        )
        self.assertEqual(call["suite"], self.env["H2_GIZCLAW_E2E_SUITE"])

    def test_make_both_wrappers(self):
        for backend in ("h2peer", "pion"):
            with self.subTest(backend=backend):
                result = self.run_command(["make", "bazel-test-gizclaw_" + backend + "_live_test"])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_live_call(self.calls()[-1], backend)

    def test_missing_endpoint_stops_before_bazel(self):
        for endpoint in (None, ""):
            for backend in ("h2peer", "pion"):
                with self.subTest(endpoint=endpoint, backend=backend):
                    self.env.pop("H2_GIZCLAW_E2E_ENDPOINT", None)
                    if endpoint is not None:
                        self.env["H2_GIZCLAW_E2E_ENDPOINT"] = endpoint
                    result = self.run_command(["sh", "scripts/bazel/bazel-test-gizclaw_" + backend + "_live_test.sh"])
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("set H2_GIZCLAW_E2E_ENDPOINT", result.stderr)
                    self.assertEqual(self.calls(), [])

    def test_endpoint_is_one_argument_without_shell_evaluation(self):
        marker = self.directory / "must-not-exist"
        for backend in ("h2peer", "pion"):
            self.env["H2_GIZCLAW_E2E_ENDPOINT"] = f"host:9821 --other; $(touch {marker})"
            result = self.run_command(["make", "bazel-test-gizclaw_" + backend + "_live_test"])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assert_live_call(self.calls()[-1], backend)
            self.assertFalse(marker.exists())
        # The production Desktop parser, not this wrapper, rejects this value.

    def test_workflow_h2peer_service(self):
        self.env["H2_GIZCLAW_E2E_SUITE"] = "service"
        result = self.workflow()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.calls()), 1)
        self.assert_live_call(self.calls()[0], "h2peer")

    def test_workflow_shared_suites(self):
        for suite in ("rpc", "firmware", "voice", "firmware-voice"):
            for backend in ("pion", "both"):
                with self.subTest(suite=suite, backend=backend):
                    before = len(self.calls())
                    self.env.update(E2E_BACKEND=backend, H2_GIZCLAW_E2E_SUITE=suite)
                    result = self.workflow()
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = ["pion"] if backend == "pion" else ["h2peer", "pion"]
                    calls = self.calls()[before:]
                    self.assertEqual(len(calls), len(expected))
                    for call, name in zip(calls, expected):
                        self.assert_live_call(call, name)

    def test_workflow_unsupported_combinations_do_not_run(self):
        for backend in ("pion", "both", "unknown"):
            for suite in ("all", "connectivity", "concurrency", "service"):
                with self.subTest(backend=backend, suite=suite):
                    self.env.update(E2E_BACKEND=backend, H2_GIZCLAW_E2E_SUITE=suite)
                    self.assertEqual(self.workflow().returncode, 2)
                    self.assertEqual(self.calls(), [])

    def test_workflow_keeps_failure_and_runs_second_backend(self):
        self.env.update(E2E_BACKEND="both", H2_GIZCLAW_E2E_SUITE="voice")
        for failed in ("h2peer", "pion"):
            with self.subTest(failed=failed):
                before = len(self.calls())
                self.env.update(COMMAND_FAIL_LABEL=PREFIX + failed + "_live_test", COMMAND_FAIL_RC="7")
                self.assertNotEqual(self.workflow().returncode, 0)
                calls = self.calls()[before:]
                self.assertEqual(len(calls), 2)
                self.assert_live_call(calls[0], "h2peer")
                self.assert_live_call(calls[1], "pion")

    def test_pal_scope_needs_no_gizclaw_endpoint(self):
        self.env["E2E_SCOPE"] = "pal"
        del self.env["H2_GIZCLAW_E2E_ENDPOINT"]
        result = self.workflow()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.calls()), 1)
        self.assertFalse(any(arg.startswith("--test_arg=") for arg in self.calls()[0]["argv"]))

    def test_workflow_missing_endpoint_and_unknown_scope(self):
        del self.env["H2_GIZCLAW_E2E_ENDPOINT"]
        for scope in ("all", "gizclaw", "unknown"):
            with self.subTest(scope=scope):
                self.env["E2E_SCOPE"] = scope
                self.assertNotEqual(self.workflow().returncode, 0)
                self.assertEqual(self.calls(), [])

    def test_all_scope_runs_pal_then_gizclaw(self):
        self.env["E2E_SCOPE"] = "all"
        result = self.workflow()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertEqual(len(calls), 2)
        self.assertIn("mqtt_public_broker_smoke", calls[0]["argv"][-1])
        self.assert_live_call(calls[1], "h2peer")
        self.env.update(COMMAND_FAIL_LABEL=calls[0]["argv"][-1], COMMAND_FAIL_RC="7")
        self.assertNotEqual(self.workflow().returncode, 0)
        self.assertEqual(len(self.calls()), 3, "PAL failure must not be hidden")


if __name__ == "__main__":
    unittest.main()
