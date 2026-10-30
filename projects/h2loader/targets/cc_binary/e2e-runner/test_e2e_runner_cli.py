import os
import subprocess
import sys
import unittest
import tempfile
from pathlib import Path

from projects.h2loader.tools.bazel.firmware_artifacts import write_package

BINARY = sys.argv[1]


class E2ERunnerCliTest(unittest.TestCase):
    def run_cli(self, *arguments, env=None):
        return subprocess.run(
            [BINARY, *arguments],
            check=False,
            capture_output=True,
            env=env,
            text=True,
        )

    def test_help_does_not_require_hardware(self):
        result = self.run_cli("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--uart ENDPOINT", result.stdout)
        self.assertIn("--ble-id ENDPOINT", result.stdout)
        self.assertIn("--coredump-bytes BYTES", result.stdout)
        self.assertIn("--baud RATE", result.stdout)
        self.assertIn("--device-logs", result.stdout)
        self.assertIn("UART baud (default 460800)", result.stdout)
        self.assertIn("--monitor-ms MS", result.stdout)
        self.assertIn("--checksum-tar-zlib DIR", result.stdout)
        self.assertIn("--checksum-zlib-tar DIR", result.stdout)

    def test_checksum_inputs_are_loaded_before_device_access(self):
        result = self.run_cli("--uart", "fake", "--checksum-zlib-tar", "/nonexistent-checksum-fixtures")
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot read checksum fixture", result.stderr)
        result = self.run_cli("--uart", "fake", "--checksum-tar-zlib", "first", "--checksum-tar-zlib", "second")
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)

    def test_app_only_lifecycle_arguments(self):
        for arguments in (("--app-lifecycle",), ("--app-lifecycle", "--app-lifecycle"),
                          ("--app-lifecycle", "--app-firmware", "a", "--loader-firmware", "b")):
            self.assertEqual(self.run_cli("--uart", "fake", *arguments).returncode, 2)
        result = self.run_cli("--uart", "fake", "--app-lifecycle", "--app-firmware", "/nonexistent-app", "--app-stage-firmware", "/nonexistent-other")
        self.assertIn("cannot read APP firmware", result.stderr)

    def test_stage_app_must_be_distinct_and_match_board(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first, other = root / "a", root / "b"
            options = dict(role="app", board="fixture", target="bk7258")
            write_package(first, "app/bk/app_ab_crc.rbl", b"first", [], version="1", **options)
            for image, version, board in ((b"first", "2", "fixture"), (b"second", "1", "fixture"), (b"second", "2", "other")):
                write_package(other, "app/bk/app_ab_crc.rbl", image, [], role="app", board=board, target="bk7258", version=version)
                result = self.run_cli("--uart", "/nonexistent-device", "--app-lifecycle", "--app-firmware", str(first), "--app-stage-firmware", str(other))
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("H2_LOADER_E2E_CASE state=start", result.stdout)

    def test_rejection_and_connection_arguments_fail_closed(self):
        for args in (("--device-logs", "--device-logs"),
                     ("--expect-loader-rejection",),
                     ("--connect-timeout-ms", "0"),
                     ("--connect-timeout-ms", "60001"),
                     ("--expect-loader-rejection", "--expect-loader-rejection")):
            result = self.run_cli("--uart", "fake", *args)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("H2_LOADER_E2E_CASE state=start", result.stdout)

    def test_endpoint_is_required(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)

    def test_url_identity_is_atomic(self):
        result = self.run_cli(
            "--uart",
            "fake",
            "--firmware-url",
            "http://example.test/update.tar.zlib",
        )
        self.assertEqual(result.returncode, 2)

    def test_repeat_must_be_positive(self):
        result = self.run_cli("--uart", "fake", "--repeat", "0")
        self.assertEqual(result.returncode, 2)

    def test_monitor_requires_uart_and_bounded_duration(self):
        result = self.run_cli(
            "--ble-id", "4:001122334455", "--monitor-ms", "500"
        )
        self.assertEqual(result.returncode, 2)
        result = self.run_cli("--uart", "fake", "--monitor-ms", "0")
        self.assertEqual(result.returncode, 2)

    def test_baud_is_a_runtime_flag(self):
        result = self.run_cli("--uart", "fake", "--baud", "0")
        self.assertEqual(result.returncode, 2)

    def test_coredump_is_single_run_and_requires_a_real_header(self):
        result = self.run_cli(
            "--uart", "fake", "--coredump-bytes", "3"
        )
        self.assertEqual(result.returncode, 2)
        result = self.run_cli(
            "--uart", "fake", "--coredump-bytes", "16384", "--repeat", "2"
        )
        self.assertEqual(result.returncode, 2)

    def test_password_is_read_from_named_environment_variable(self):
        environment = os.environ.copy()
        environment.pop("H2_E2E_TEST_PASSWORD_MISSING", None)
        result = self.run_cli(
            "--uart",
            "fake",
            "--wifi-ssid",
            "test-network",
            "--wifi-password-env",
            "H2_E2E_TEST_PASSWORD_MISSING",
            env=environment,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("environment variable is not set", result.stderr)
        self.assertNotIn("test-network", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
