#!/usr/bin/env python3
"""Install the packaged App, require all portable Core cases, retain evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import zipfile

PACKAGE = "com.haivivi.gizos.e2e.palcore"


def run(argv, check=True, timeout=45):
    result = subprocess.run([str(a) for a in argv], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(f"{argv}: {result.stdout}")
    return result


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_CORE_CASE\("([^"]+)"\)', registry.read_text())
    assert len(ids) == 41 and len(set(ids)) == 41, "unexpected contract registry"
    expected = dict(platform=platform, version="2.0.0", contract=2, passed=len(ids),
                    failed=0, blocked=0, not_run=0, complete=1, qualified=1,
                    cleanup=0, rc=0, teardown=0, balanced=1)
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case["rc"] == 0 for case in report["cases"])
    assert report["before"] == report["after"], "resource leak"


def wait_report(read, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = read()
        try:
            return json.loads(raw)
        except (ValueError, TypeError):
            time.sleep(0.25)
    raise TimeoutError(f"No complete report after {timeout}s")


def ios(args, output):
    device = os.environ.get("H2_IOS_SIMULATOR_UDID")
    if not device:
        raise ValueError("H2_IOS_SIMULATOR_UDID must identify a booted test simulator")
    sim = ["xcrun", "simctl"]
    with tempfile.TemporaryDirectory(prefix="pal-core-ios-") as temporary:
        with zipfile.ZipFile(args.app) as archive:
            archive.extractall(temporary)
        apps = list(Path(temporary).glob("Payload/*.app"))
        assert len(apps) == 1
        run(sim + ["terminate", device, PACKAGE], check=False)
        run(sim + ["install", device, apps[0]])
    container = Path(run(sim + ["get_app_container", device, PACKAGE, "data"]).stdout.strip())
    result_file = container / "Documents/pal-core-result.json"
    result_file.unlink(missing_ok=True)
    # Simulator redirects these files inside its own data directory.
    stdout = container / "Documents/pal-core.stdout"
    stderr = container / "Documents/pal-core.stderr"
    run(sim + ["launch", f"--stdout={stdout}", f"--stderr={stderr}", device, PACKAGE])
    try:
        result = wait_report(lambda: result_file.read_text() if result_file.exists() else "", args.timeout)
    finally:
        run(sim + ["terminate", device, PACKAGE], check=False)
        for source in (stdout, stderr):
            if source.exists():
                (output / source.name).write_bytes(source.read_bytes())
    # Device/runtime identity comes from simctl, not an assumed host OS version.
    devices = json.loads(run(sim + ["list", "devices", "--json"]).stdout)["devices"]
    identity = next({"runtime": runtime, "device": value} for runtime, values in devices.items()
                    for value in values if value["udid"] == device)
    return result, identity


def android(args, output):
    serial = os.environ.get("H2_ANDROID_SERIAL")
    if not serial or not serial.startswith("emulator-"):
        raise ValueError("H2_ANDROID_SERIAL must explicitly identify the test emulator")
    sdk = os.environ.get("ANDROID_HOME")
    adb = [str(Path(sdk) / "platform-tools/adb") if sdk else "adb", "-s", serial]
    run(adb + ["shell", "am", "force-stop", PACKAGE])
    run(adb + ["install", "-r", args.app])
    run(adb + ["shell", "run-as", PACKAGE, "rm", "-f", "files/pal-core-result.json"])
    run(adb + ["shell", "am", "start", "-W", "-n", PACKAGE + "/.MainActivity"])
    try:
        result = wait_report(lambda: run(adb + ["shell", "run-as", PACKAGE, "cat",
                                                 "files/pal-core-result.json"], check=False).stdout, args.timeout)
    finally:
        log = run(adb + ["shell", "run-as", PACKAGE, "cat", "files/pal-core-result.json.log"], check=False)
        (output / "pal-core.stdout").write_text(log.stdout)
        pid = run(adb + ["shell", "pidof", PACKAGE], check=False).stdout.strip()
        if pid.isdigit():
            (output / "logcat.txt").write_text(run(adb + ["logcat", "-d", "--pid=" + pid]).stdout)
        run(adb + ["shell", "am", "force-stop", PACKAGE])
    with zipfile.ZipFile(args.app) as app, zipfile.ZipFile(args.sdk) as sdk_archive:
        assert app.read("lib/arm64-v8a/libh2_pal_core.so") == sdk_archive.read("jni/arm64-v8a/libh2_pal_core.so"), "APK did not use AAR binary"
    return result, {"serial": serial, "api": run(adb + ["shell", "getprop", "ro.build.version.sdk"]).stdout.strip(),
                    "fingerprint": run(adb + ["shell", "getprop", "ro.build.fingerprint"]).stdout.strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-core-mobile-result")))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result, environment = (ios if args.platform == "ios" else android)(args, args.output)
    (args.output / "qualified.json").write_text(json.dumps(result, indent=2) + "\n")
    environment.update(app_sha256=hashlib.sha256(args.app.read_bytes()).hexdigest(),
                       sdk_sha256=hashlib.sha256(args.sdk.read_bytes()).hexdigest())
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    verify(result, args.registry, "ios-simulator" if args.platform == "ios" else "android-emulator")
    print(f"PAL Core {args.platform}: 41/41 PASS, blocked=0, cleanup=0, balanced=1")

if __name__ == "__main__":
    main()
