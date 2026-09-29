"""Failure and lifecycle contracts for the shared mobile E2E host runner."""

import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import subprocess
import sys
import time
from unittest.mock import patch
import unittest
import zipfile

from tools.bazel.mobile_e2e import MobileApp, run_command, save_evidence, wait_report


def archive(path, entries):
    with zipfile.ZipFile(path, "w") as target:
        for name, value in entries.items():
            target.writestr(name, value)


class FakeDevice:
    def __init__(self, container=None, reports=None, fail=None):
        self.container = container
        self.reports = list(reports or [])
        self.fail = fail
        self.calls = []
        self.report = ""

    def __call__(self, argv, **kwargs):
        parts = [str(value) for value in argv]
        self.calls.append((parts, kwargs))
        if self.fail and self.fail in parts:
            raise RuntimeError("injected " + self.fail + " failure")
        stdout = ""
        if "get_app_container" in parts:
            stdout = str(self.container)
        elif parts[-3:] == ["list", "devices", "--json"]:
            stdout = json.dumps({"devices": {"iOS": [{"udid": "SIM-1", "state": "Booted"}]}})
        elif "launch" in parts or "start" in parts:
            self.report = self.reports.pop(0) if self.reports else ""
            if "launch" in parts and self.report:
                (self.container / "Documents" / "result.json").write_text(self.report)
        elif "cat" in parts and parts[-1] == "files/result.json":
            stdout = self.report
        elif "getprop" in parts:
            stdout = "35"
        elif "pidof" in parts:
            stdout = ""
        return SimpleNamespace(stdout=stdout, returncode=0)


class MobileRunnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.container = self.root / "container"
        (self.container / "Documents").mkdir(parents=True)
        self.ipa = self.root / "app.ipa"
        self.apk = self.root / "app.apk"
        self.aar = self.root / "sdk.aar"
        self.sdk = self.root / "sdk.zip"
        archive(self.ipa, {"Payload/Test.app/Test": b"binary"})
        archive(self.apk, {"lib/arm64-v8a/libh2_pal_core.so": b"provider"})
        archive(self.aar, {"jni/arm64-v8a/libh2_pal_core.so": b"provider"})
        archive(self.sdk, {"sdk": b"provider"})

    def ios(self, fake, timeout=1):
        return MobileApp("ios", self.ipa, self.sdk, "com.test", "result.json",
                         self.root / "out", timeout=timeout, command=fake,
                         environ={"H2_IOS_SIMULATOR_UDID": "SIM-1"}, prefix="test")

    def android(self, fake, timeout=1):
        return MobileApp("android", self.apk, self.aar, "com.test", "result.json",
                         self.root / "out", timeout=timeout, command=fake,
                         environ={"H2_ANDROID_SERIAL": "emulator-5554"}, prefix="test")

    def test_ios_install_launch_evidence_and_cleanup(self):
        fake = FakeDevice(self.container, ['{"passed": 1}'])
        with self.ios(fake) as app:
            self.assertTrue(app.ios_app.is_dir())
            self.assertEqual(app.launch(), {"passed": 1})
            environment = app.environment()
            self.assertEqual(environment["device"]["udid"], "SIM-1")
        self.assertTrue(any("terminate" in call for call, _ in fake.calls))
        self.assertIsNone(app._temporary)
        save_evidence(self.root / "out", {"passed": 1}, environment, self.ipa, self.sdk)
        self.assertEqual(json.loads((self.root / "out/qualified.json").read_text())["passed"], 1)
        self.assertEqual(len(json.loads((self.root / "out/environment.json").read_text())["app_sha256"]), 64)

    def test_install_failure_releases_temporary_ipa(self):
        fake = FakeDevice(self.container, fail="install")
        app = self.ios(fake)
        with self.assertRaisesRegex(RuntimeError, "injected install"):
            with app:
                pass
        self.assertIsNone(app._temporary)

    def test_start_failure_still_stops_app(self):
        fake = FakeDevice(self.container, fail="launch")
        with self.ios(fake) as app:
            with self.assertRaisesRegex(RuntimeError, "injected launch"):
                app.launch()
        self.assertGreaterEqual(sum("terminate" in call for call, _ in fake.calls), 2)

    def test_bad_report_times_out_and_android_stops(self):
        fake = FakeDevice(reports=["not-json"])
        with self.android(fake, timeout=0) as app:
            with self.assertRaises(TimeoutError):
                app.launch()
        self.assertGreaterEqual(sum("force-stop" in call for call, _ in fake.calls), 2)

    def test_android_two_process_phases_and_binary_identity(self):
        fake = FakeDevice(reports=['{"phase":1}', '{"phase":2}'])
        with self.android(fake) as app:
            phases = [app.launch(android_args=("--ei", "phase", str(phase))) for phase in (1, 2)]
            identity = app.verify_android_sdk()
        self.assertEqual([item["phase"] for item in phases], [1, 2])
        self.assertTrue(identity["apk_aar_binary_identical"])
        self.assertEqual(sum("start" in call for call, _ in fake.calls), 2)
        self.assertGreaterEqual(sum("force-stop" in call for call, _ in fake.calls), 3)

    def test_wrong_aar_binary_fails_identity(self):
        archive(self.aar, {"jni/arm64-v8a/libh2_pal_core.so": b"different"})
        fake = FakeDevice()
        with self.assertRaisesRegex(AssertionError, "APK did not use AAR binary"):
            with self.android(fake):
                pass

    def test_container_failure_after_install_stops_and_records_error(self):
        fake = FakeDevice(self.container, fail="get_app_container")
        app = self.ios(fake)
        with self.assertRaisesRegex(RuntimeError, "get_app_container"):
            with app:
                pass
        self.assertIn("terminate", fake.calls[-1][0])
        self.assertIsNone(app._temporary)
        failure = json.loads((self.root / "out/failure.json").read_text())
        self.assertIn("get_app_container", failure["error"])
        self.assertEqual(json.loads((self.root / "out/environment.json").read_text())["runner_status"], "failed")

    def test_log_failure_never_skips_stop_or_masks_original_error(self):
        fake = FakeDevice(reports=["bad-json"])
        original = fake.__call__
        def command(argv, **kwargs):
            if "pidof" in argv:
                raise RuntimeError("log capture failed")
            return original(argv, **kwargs)
        app = self.android(command, timeout=0.01)
        with self.assertRaises(TimeoutError):
            with app:
                app.launch()
        self.assertIn("force-stop", fake.calls[-1][0])
        failure = json.loads((self.root / "out/failure.json").read_text())
        self.assertIn("TimeoutError", failure["error"])
        self.assertTrue(any("log capture failed" in item for item in failure["cleanup_errors"]))
        self.assertEqual((self.root / "out/result.json").read_text(), "bad-json")

    def test_cleanup_failure_fails_an_otherwise_successful_run(self):
        fake = FakeDevice(reports=['{"passed":1}'])
        with self.assertRaisesRegex(RuntimeError, "cleanup/evidence failed"):
            with self.android(fake) as app:
                app.launch()
                fake.fail = "force-stop"
        self.assertTrue((self.root / "out/failure.json").exists())

    def test_ios_termination_nonzero_is_only_ignored_for_already_exited_app(self):
        fake = FakeDevice(self.container)
        with self.assertRaisesRegex(RuntimeError, "simctl terminate failed"):
            with self.ios(fake) as app:
                original = app.command
                def command(argv, **kwargs):
                    if "terminate" in argv:
                        return SimpleNamespace(returncode=1, stdout="device unavailable")
                    return original(argv, **kwargs)
                app.command = command
        self.assertIn("device unavailable", (self.root / "out/failure.json").read_text())

    def test_fixture_and_app_cleanup_on_oracle_failure(self):
        fake = FakeDevice(self.container, ['{"passed":0}'])
        with self.assertRaisesRegex(AssertionError, "suite failed"):
            with self.ios(fake) as app:
                with app.fixture("fixture.json", "{}"):
                    app.launch()
                    raise AssertionError("suite failed")
        self.assertFalse((self.container / "Documents/fixture.json").exists())
        self.assertEqual(json.loads((self.root / "out/result.json").read_text())["passed"], 0)
        self.assertIn("suite failed", (self.root / "out/failure.json").read_text())

    def test_android_first_launch_creates_image_directory_before_listing(self):
        fake = FakeDevice(reports=['{"passed":1}'])
        original = fake.__call__
        created = False
        def command(argv, **kwargs):
            nonlocal created
            if "mkdir" in argv:
                created = True
            if "ls" in argv:
                if not created:
                    raise RuntimeError("files: No such file or directory")
            return original(argv, **kwargs)
        with self.android(command) as app:
            self.assertEqual(app.launch(capture_png=True), {"passed": 1})
        self.assertTrue(created)

    def test_stale_report_is_removed_and_phases_keep_distinct_evidence(self):
        stale = self.container / "Documents/result.json"
        stale.write_text('{"phase":99}')
        fake = FakeDevice(self.container, ['{"phase":1}', '{"phase":2}'])
        original = fake.__call__
        def command(argv, **kwargs):
            if "launch" in argv:
                self.assertFalse(stale.exists())
            return original(argv, **kwargs)
        with self.ios(command) as app:
            self.assertEqual(app.launch()["phase"], 1)
            self.assertEqual(app.launch()["phase"], 2)
        self.assertEqual(json.loads((self.root / "out/result.json").read_text())["phase"], 1)
        self.assertEqual(json.loads((self.root / "out/result-2.json").read_text())["phase"], 2)
        lifecycle = ["start" if "launch" in call else "stop" for call, _ in fake.calls if "launch" in call or "terminate" in call]
        self.assertEqual(lifecycle, ["stop", "start", "stop", "start", "stop", "stop"])

    def test_incomplete_report_retried_but_oracle_errors_are_not(self):
        reports = iter(["", '{"passed":', '{"passed":1}'])
        with patch("tools.bazel.mobile_e2e.time.sleep"):
            self.assertEqual(wait_report(lambda: next(reports), 1), {"passed": 1})
        with self.assertRaisesRegex(AssertionError, "bad ledger"):
            wait_report(lambda: "{}", 1, lambda _: (_ for _ in ()).throw(AssertionError("bad ledger")))

    def test_launch_command_uses_remaining_deadline_and_timeout_still_stops(self):
        fake = FakeDevice()
        original = fake.__call__
        def command(argv, **kwargs):
            if "start" in argv:
                self.assertLessEqual(kwargs["timeout"], 0.5)
                raise subprocess.TimeoutExpired(argv, kwargs["timeout"])
            return original(argv, **kwargs)
        with self.assertRaises(subprocess.TimeoutExpired):
            with self.android(command, timeout=0.5) as app:
                app.launch()
        self.assertIn("force-stop", fake.calls[-1][0])

    def test_run_command_real_subprocess_timeout(self):
        started = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            run_command([sys.executable, "-c", "import time; time.sleep(10)"], timeout=0.05)
        self.assertLess(time.monotonic() - started, 5)

    def test_ios_provider_probe_rejects_missing_or_substring_symbol(self):
        member = "H2PALCore.xcframework/ios-arm64-simulator/H2PALCore.framework/"
        archive(self.sdk, {member + "H2PALCore": b"provider", member + "Headers/h2_ios_platform.h": b"header"})
        fake = FakeDevice(self.container)
        with self.ios(fake) as app:
            original = app.command
            def command(argv, **kwargs):
                if "nm" in argv:
                    return SimpleNamespace(stdout="000 T _provider_wrong\n000 T _portable\n", returncode=0)
                return original(argv, **kwargs)
            app.command = command
            with self.assertRaisesRegex(AssertionError, "provider symbols"):
                app.verify_ios_symbols(executable="Test", provider_symbols=("_provider",), portable_symbol="_portable")

    def test_android_provider_probe_requires_header_and_defined_export(self):
        header = "prefab/modules/h2_pal_core/include/provider.h"
        archive(self.aar, {"jni/arm64-v8a/libh2_pal_core.so": b"provider", header: b"header"})
        fake = FakeDevice()
        with self.android(fake) as app:
            app.environ["ANDROID_NDK_HOME"] = "/fake/ndk"
            original = app.command
            def command(argv, **kwargs):
                if "--dynamic" in argv:
                    return SimpleNamespace(stdout=" U provider\n", returncode=0)
                return original(argv, **kwargs)
            app.command = command
            with self.assertRaisesRegex(AssertionError, "not public"):
                app.verify_android_sdk(required_header=header, public_symbols=("provider",))

    def test_missing_explicit_simulator_is_rejected(self):
        app = MobileApp("ios", self.ipa, self.sdk, "com.test", "result.json",
                        self.root / "out", command=FakeDevice(), environ={})
        with self.assertRaisesRegex(ValueError, "H2_IOS_SIMULATOR_UDID"):
            with app:
                pass


if __name__ == "__main__":
    unittest.main()
