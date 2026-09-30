"""Shared host-side lifecycle for packaged iOS and Android E2E Apps.

Bazel declares suite identity, report contracts and SDK probes. Optional Python
hooks own only special fixtures, phase plans, parsers and business assertions.
"""

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import zipfile


def run_command(argv, check=True, timeout=45, input=None, text=True):
    result = subprocess.run([str(arg) for arg in argv], text=text,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout, input=input)
    if check and result.returncode:
        raise RuntimeError(f"{argv}: {result.stdout}")
    return result


def wait_report(read, timeout, parse=json.loads):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return parse(read())
        except (ValueError, TypeError, FileNotFoundError):
            time.sleep(min(0.25, max(0, deadline - time.monotonic())))
    raise TimeoutError(f"No complete report after {timeout}s")


def save_evidence(output, report, environment, app, sdk, *, report_name="qualified.json"):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / report_name).write_text(json.dumps(report, indent=2) + "\n")
    environment.update(observation_finished_at_utc=datetime.now(timezone.utc).isoformat(),
                       evidence_timezone="UTC",
                       app_sha256=hashlib.sha256(Path(app).read_bytes()).hexdigest(),
                       sdk_sha256=hashlib.sha256(Path(sdk).read_bytes()).hexdigest())
    (output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")


class MobileApp:
    """One installed App; launch() may run more than once for persistence tests."""

    def __init__(self, platform, app, sdk, package, result_name, output, *,
                 timeout=90, command=run_command, environ=None, prefix=None):
        if platform not in ("ios", "android"):
            raise ValueError(f"unsupported mobile platform: {platform}")
        self.platform = platform
        self.app = Path(app)
        self.sdk = Path(sdk)
        self.package = package
        self.result_name = result_name
        self.output = Path(output)
        self.timeout = timeout
        self.command = command
        self.environ = os.environ if environ is None else environ
        self.prefix = prefix or result_name.removesuffix("-result.json").removesuffix("-result.log")
        self.device = None
        self.adb = None
        self.container = None
        self.ios_app = None
        self._temporary = None
        self._installed = False
        self._launch_count = 0
        self._deadline = None
        self.identity = {"platform": platform, "package": package,
                         "observation_started_at_utc": datetime.now(timezone.utc).isoformat()}
        self.cleanup_errors = []

    def _run(self, argv, **kwargs):
        if self._deadline is not None:
            remaining = self._deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"App launch/report exceeded {self.timeout}s")
            kwargs["timeout"] = min(kwargs.get("timeout", 45), remaining)
        return self.command(argv, **kwargs)

    def simctl(self, *args, **kwargs):
        return self._run(["xcrun", "simctl", *args], **kwargs)

    def adb_command(self, *args, **kwargs):
        return self._run([*self.adb, *args], **kwargs)

    def __enter__(self):
        self.output.mkdir(parents=True, exist_ok=True)
        for name in ("qualified.json", "failure.json", "environment.json"):
            (self.output / name).unlink(missing_ok=True)
        try:
            self.identity.update(app_sha256=hashlib.sha256(self.app.read_bytes()).hexdigest(),
                                 sdk_sha256=hashlib.sha256(self.sdk.read_bytes()).hexdigest())
            if self.platform == "ios":
                self.device = self.environ.get("H2_IOS_SIMULATOR_UDID")
                if not self.device:
                    raise ValueError("H2_IOS_SIMULATOR_UDID must identify a booted test simulator")
                self.identity["simulator_udid"] = self.device
                self._temporary = tempfile.TemporaryDirectory(prefix=self.prefix + "-ios-")
                with zipfile.ZipFile(self.app) as archive:
                    archive.extractall(self._temporary.name)
                apps = list(Path(self._temporary.name).glob("Payload/*.app"))
                if len(apps) != 1:
                    raise ValueError("IPA must contain exactly one App")
                self.ios_app = apps[0]
                self.simctl("terminate", self.device, self.package, check=False)
                self._installed = True  # Also stop after a partially failed install.
                self.simctl("install", self.device, self.ios_app)
                container = self.simctl("get_app_container", self.device, self.package, "data").stdout.strip()
                if not container or not Path(container).is_absolute():
                    raise ValueError(f"invalid simulator data container: {container!r}")
                self.container = Path(container)
                (self.container / "Documents").mkdir(exist_ok=True)
            else:
                self.device = self.environ.get("H2_ANDROID_SERIAL")
                if not self.device or not self.device.startswith("emulator-"):
                    raise ValueError("H2_ANDROID_SERIAL must explicitly identify the test emulator")
                self.identity["serial"] = self.device
                sdk_home = self.environ.get("ANDROID_HOME")
                self.adb = [str(Path(sdk_home) / "platform-tools/adb") if sdk_home else "adb", "-s", self.device]
                self.adb_command("shell", "am", "force-stop", self.package, check=False)
                self.identity.update(self.verify_android_sdk())
                self._installed = True
                self.adb_command("install", "-r", self.app)
            self.identity.update(self._device_environment())
            return self
        except BaseException as error:
            self.close(error)
            raise

    def __exit__(self, _type, value, _traceback):
        self.close(value)

    def _cleanup(self, operation):
        try:
            operation()
        except Exception as error:
            self.cleanup_errors.append(f"{type(error).__name__}: {error}")

    def stop(self):
        if self.platform == "ios":
            result = self.simctl("terminate", self.device, self.package, check=False)
            # An already exited App is harmless; other termination failures
            # must not turn a leaked process into a successful qualification.
            if result.returncode and "found nothing to terminate" not in result.stdout:
                raise RuntimeError(f"simctl terminate failed: {result.stdout}")
        else:
            self.adb_command("shell", "am", "force-stop", self.package)

    def close(self, error=None):
        self._deadline = None
        if self._installed:
            self._cleanup(self.stop)
            self._installed = False
        if self._temporary is not None:
            self._cleanup(self._temporary.cleanup)
            self._temporary = None
        self.identity.update(observation_finished_at_utc=datetime.now(timezone.utc).isoformat(),
                             evidence_timezone="UTC", launches=self._launch_count,
                             runner_status="failed" if error or self.cleanup_errors else "completed")
        if self.cleanup_errors:
            self.identity["cleanup_errors"] = self.cleanup_errors
        if error or self.cleanup_errors:
            failure = {"error": f"{type(error).__name__}: {error}" if error else "cleanup failed",
                       "cleanup_errors": self.cleanup_errors, "launches": self._launch_count}
            self._cleanup(lambda: (self.output / "failure.json").write_text(json.dumps(failure, indent=2) + "\n"))
            # A report that passed before cleanup failed must not look qualified.
            self._cleanup(lambda: (self.output / "qualified.json").unlink(missing_ok=True))
        self._cleanup(lambda: (self.output / "environment.json").write_text(json.dumps(self.identity, indent=2) + "\n"))
        if error is None and self.cleanup_errors:
            raise RuntimeError("mobile cleanup/evidence failed: " + "; ".join(self.cleanup_errors))

    def result_path(self):
        if self.platform == "ios":
            return self.container / "Documents" / self.result_name
        return "files/" + self.result_name

    def clear_result(self):
        if self.platform == "ios":
            self.result_path().unlink(missing_ok=True)
        else:
            self.adb_command("shell", "run-as", self.package, "rm", "-f",
                             self.result_path(), self.result_path() + ".log")

    def read_result(self):
        if self.platform == "ios":
            path = self.result_path()
            return path.read_text() if path.exists() else ""
        return self.adb_command("shell", "run-as", self.package, "cat", self.result_path(),
                                check=False).stdout

    def write_fixture(self, name, content):
        if self.platform == "ios":
            (self.container / "Documents" / name).write_text(content)
        else:
            self.adb_command("shell", "run-as", self.package, "mkdir", "-p", "files")
            self.adb_command("shell", "run-as", self.package, "tee", "files/" + name,
                             input=content)

    def remove_fixture(self, name):
        if self.platform == "ios":
            (self.container / "Documents" / name).unlink(missing_ok=True)
        else:
            self.adb_command("shell", "run-as", self.package, "rm", "-f", "files/" + name)

    @contextmanager
    def fixture(self, name, content):
        """Remove even a partially written fixture without hiding the test error."""
        error = None
        try:
            self.write_fixture(name, content)
            yield
        except BaseException as caught:
            error = caught
            raise
        finally:
            self._cleanup(lambda: self.remove_fixture(name))
            if error is None and self.cleanup_errors:
                raise RuntimeError("fixture cleanup failed: " + "; ".join(self.cleanup_errors))

    def launch(self, *, parse=json.loads, ios_args=(), android_args=(),
               capture_png=False, android_log=None):
        """One deadline covers launch and polling; every attempt ends with stop.

        Parsers raise ValueError for an incomplete report. Oracle assertions belong
        to the suite after launch returns, inside the MobileApp context.
        """
        suffix = "" if self._launch_count == 0 else f"-{self._launch_count + 1}"
        self._launch_count += 1
        error = None
        stdout = self.container / "Documents" / (self.prefix + suffix + ".stdout") if self.container else None
        stderr = self.container / "Documents" / (self.prefix + suffix + ".stderr") if self.container else None
        try:
            self._deadline = time.monotonic() + self.timeout
            self.clear_result()
            if self.platform == "ios":
                for source in (stdout, stderr):
                    source.unlink(missing_ok=True)
                if capture_png:
                    for source in (self.container / "Documents").glob("*.png"):
                        source.unlink()
                self.simctl("launch", f"--stdout={stdout}", f"--stderr={stderr}",
                            self.device, self.package, *ios_args)
            else:
                if capture_png:
                    # A fresh install has no files/ until the Activity first runs.
                    self.adb_command("shell", "run-as", self.package, "mkdir", "-p", "files")
                    for name in self.adb_command("shell", "run-as", self.package, "ls", "files").stdout.splitlines():
                        if name.endswith(".png"):
                            self.remove_fixture(name)
                self.adb_command("shell", "am", "start", "-W", "-n",
                                 self.package + "/.MainActivity", *android_args)
            return wait_report(self.read_result, max(0, self._deadline - time.monotonic()), parse)
        except BaseException as caught:
            error = caught
            raise
        finally:
            self._deadline = None
            # Each collection is independent; even a broken adb/logcat must stop.
            if self.platform == "ios":
                self._cleanup(self.stop)
                for source in (stdout, stderr):
                    self._cleanup(lambda source=source: self._copy_log(source))
            else:
                self._cleanup(lambda: self._android_logs(suffix, android_log))
            self._cleanup(lambda: (self.output / (Path(self.result_name).stem + suffix + Path(self.result_name).suffix)).write_text(self.read_result()))
            if capture_png:
                self._cleanup(self._capture_png)
            if self.platform == "android":
                self._cleanup(self.stop)
            if error is None and self.cleanup_errors:
                raise RuntimeError("mobile evidence/cleanup failed: " + "; ".join(self.cleanup_errors))

    def _copy_log(self, source):
        if source.exists():
            (self.output / source.name).write_bytes(source.read_bytes())

    def _android_logs(self, suffix, log_name):
        log_name = log_name or self.result_name + ".log"
        log = self.adb_command("shell", "run-as", self.package, "cat", "files/" + log_name, check=False)
        (self.output / (self.prefix + suffix + ".stdout")).write_text(log.stdout)
        pid = self.adb_command("shell", "pidof", self.package, check=False).stdout.strip()
        if pid.isdigit():
            (self.output / ("logcat" + suffix + ".txt")).write_text(
                self.adb_command("logcat", "-d", "--pid=" + pid).stdout)

    def _capture_png(self):
        if self.platform == "ios":
            for source in (self.container / "Documents").glob("*.png"):
                self._copy_log(source)
        else:
            for name in self.adb_command("shell", "run-as", self.package, "ls", "files").stdout.splitlines():
                if name.endswith(".png"):
                    capture = self.adb_command("exec-out", "run-as", self.package,
                                               "cat", "files/" + name, text=False)
                    (self.output / name).write_bytes(capture.stdout)

    def environment(self):
        return self.identity

    def _device_environment(self):
        if self.platform == "ios":
            devices = json.loads(self.simctl("list", "devices", "--json").stdout)["devices"]
            return next({"runtime": runtime, "device": value}
                        for runtime, values in devices.items() for value in values
                        if value["udid"] == self.device)
        return {"serial": self.device,
                "api": self.adb_command("shell", "getprop", "ro.build.version.sdk").stdout.strip(),
                "fingerprint": self.adb_command("shell", "getprop", "ro.build.fingerprint").stdout.strip()}

    def verify_android_sdk(self, *, required_header=None, public_symbols=()):
        """Require that the APK actually carries the AAR's arm64 provider."""
        with zipfile.ZipFile(self.app) as app, zipfile.ZipFile(self.sdk) as aar:
            apk_binary = app.read("lib/arm64-v8a/libh2_pal_core.so")
            sdk_binary = aar.read("jni/arm64-v8a/libh2_pal_core.so")
            if apk_binary != sdk_binary:
                raise AssertionError("APK did not use AAR binary")
            if required_header and not aar.read(required_header):
                raise AssertionError("AAR exported header is empty")
        details = {"aar_binary_sha256": hashlib.sha256(sdk_binary).hexdigest(),
                   "apk_binary_sha256": hashlib.sha256(apk_binary).hexdigest(),
                   "apk_aar_binary_identical": True}
        if public_symbols:
            ndk = self.environ.get("ANDROID_NDK_HOME")
            if not ndk:
                raise ValueError("ANDROID_NDK_HOME is required for AAR exported-symbol verification")
            host = "darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64"
            with tempfile.TemporaryDirectory(prefix=self.prefix + "-aar-symbols-") as directory:
                binary = Path(directory) / "libh2_pal_core.so"
                binary.write_bytes(sdk_binary)
                llvm_nm = Path(ndk) / "toolchains/llvm/prebuilt" / host / "bin/llvm-nm"
                exported = self._run([llvm_nm, "--dynamic", binary]).stdout
            for symbol in public_symbols:
                if not re.search(r"\bT " + re.escape(symbol) + r"\b", exported):
                    raise AssertionError(f"AAR symbol is not public: {symbol}")
            details["public_provider_symbols"] = list(public_symbols)
        self.identity.update(details)
        return details

    def verify_ios_symbols(self, *, executable, provider_symbols, portable_symbol,
                           header_member="H2PALCore.xcframework/ios-arm64-simulator/H2PALCore.framework/Headers/h2_ios_platform.h"):
        """Require suite provider symbols in both XCFramework and linked IPA."""
        app_executable = self.ios_app / executable
        with zipfile.ZipFile(self.sdk) as archive:
            member = "H2PALCore.xcframework/ios-arm64-simulator/H2PALCore.framework/H2PALCore"
            sdk_binary = archive.read(member)
            if not archive.read(header_member):
                raise AssertionError("XCFramework exported header is empty")
        with tempfile.TemporaryDirectory(prefix=self.prefix + "-sdk-symbols-") as directory:
            binary = Path(directory) / "H2PALCore.a"
            binary.write_bytes(sdk_binary)
            sdk_symbols = self._run(["xcrun", "nm", "-gU", binary]).stdout
        app_symbols = self._run(["xcrun", "nm", "-gU", app_executable]).stdout
        def defined(symbol, symbols):
            return re.search(r"(?m)\b[TDS] " + re.escape(symbol) + r"$", symbols) is not None
        if not all(defined(symbol, sdk_symbols) and defined(symbol, app_symbols) for symbol in provider_symbols):
            raise AssertionError("packaged provider symbols are missing")
        if not defined(portable_symbol, app_symbols):
            raise AssertionError("portable App symbol missing from IPA")
        details = {"sdk_binary_member": member,
                "sdk_binary_sha256": hashlib.sha256(sdk_binary).hexdigest(),
                "ipa_executable_sha256": hashlib.sha256(app_executable.read_bytes()).hexdigest(),
                "provider_symbols_in_sdk_and_ipa": list(provider_symbols)}
        self.identity.update(details)
        return details


def registry_ids(contract, registry):
    matches = list(re.finditer(contract["registry_pattern"], registry.read_text()))
    ids = [match.group(1) for match in matches]
    assert ids and len(ids) == len(set(ids)), "invalid contract registry"
    assert not contract["case_count"] or len(ids) == contract["case_count"], "unexpected contract registry"
    return ids


def verify_report(report, contract, ids, platform):
    """A small fixed report contract: exact fields, ordered PASS ledger, balance."""
    expected = dict(contract["expected"], platform=platform, passed=len(ids))
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case[contract["case_result"]] == 0 for case in report["cases"])
    if contract["resource_balance"]:
        assert report["before"] == report["after"], "resource leak"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, required=True)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int)
    args = parser.parse_args(argv)
    contract = args.contract = json.loads(args.suite.read_text())
    args.registry = Path(contract["registry"])
    ids = registry_ids(contract, args.registry)
    args.fixtures = {name: Path(value) for name, value in contract["fixtures"].items()}
    hook = None
    if contract["hook"]:
        spec = importlib.util.spec_from_file_location("mobile_hook", contract["hook"])
        hook = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = hook
        spec.loader.exec_module(hook)
    prefix = contract["prefix"] or Path(contract["report"]).stem.removesuffix("-result")
    args.output = args.output or Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR") or
                                     contract["output_default"] or f"/tmp/{prefix}-mobile-result")
    args.report_platform = args.platform if contract["plain_platform"] else (
        "ios-simulator" if args.platform == "ios" else "android-emulator")
    # A live service suite must reject missing operator inputs before install,
    # fixture writes, permission changes or App/provider initialization.
    if hasattr(hook, "preflight"):
        hook.preflight(args)
    with MobileApp(args.platform, args.app, args.sdk, contract["package"], contract["report"],
                   args.output, timeout=args.timeout if args.timeout is not None else contract["timeout"], prefix=prefix) as app:
        app.environment()["suite_sha256"] = hashlib.sha256(args.suite.read_bytes()).hexdigest()
        if contract[args.platform + "_sdk"]:
            probe = app.verify_ios_symbols if args.platform == "ios" else app.verify_android_sdk
            probe(**contract[args.platform + "_sdk"])
        for permission in contract["permissions"][args.platform]:
            if args.platform == "ios":
                app.simctl("privacy", app.device, "grant", permission, app.package)
            else:
                app.adb_command("shell", "pm", "grant", app.package, permission)
        if hasattr(hook, "run_suite"):
            report = hook.run_suite(app, args)
        else:
            report = app.launch(parse=getattr(hook, "parse_report", json.loads),
                                capture_png=contract["capture_png"], android_log=contract["android_log"])
        verify_report(report, contract, ids, args.report_platform)
        if hasattr(hook, "verify_report"):
            hook.verify_report(report, args)
        save_evidence(args.output, report, app.environment(), args.app, args.sdk)
    print(f"{prefix} {args.platform}: {report['passed']}/{len(ids)} PASS")


if __name__ == "__main__":
    main()
