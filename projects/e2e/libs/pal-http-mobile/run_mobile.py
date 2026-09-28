#!/usr/bin/env python3
"""Install the packaged App, require all portable HTTP cases, retain evidence."""
from datetime import datetime, timezone
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
import sys

sys.path.insert(0, str(Path(__file__).absolute().parents[4] / "projects/e2e/libs/pal-http-fixture"))
from fixture import Fixture

PACKAGE = "com.haivivi.gizos.e2e.palhttp"


def run(argv, check=True, timeout=45, input=None):
    result = subprocess.run([str(a) for a in argv], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout, input=input)
    if check and result.returncode:
        raise RuntimeError(f"{argv}: {result.stdout}")
    return result


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_HTTP_CASE\(\w+, "([^"]+)"\)', registry.read_text())
    assert len(ids) == 45 and len(set(ids)) == 45, "unexpected contract registry"
    expected = dict(platform=platform, version="2.0.0", contract=1, operations=2, passed=len(ids),
                    failed=0, blocked=0, retained_allocations=0, rc=0, teardown=0)
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case["detail"] == 0 for case in report["cases"])


def wait_report(read, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = read()
        try:
            return json.loads(raw)
        except (ValueError, TypeError):
            time.sleep(0.25)
    raise TimeoutError(f"No complete report after {timeout}s")


def ios(args, output, fixture):
    device = os.environ.get("H2_IOS_SIMULATOR_UDID")
    if not device:
        raise ValueError("H2_IOS_SIMULATOR_UDID must identify a booted test simulator")
    sim = ["xcrun", "simctl"]
    with tempfile.TemporaryDirectory(prefix="pal-http-ios-") as temporary:
        with zipfile.ZipFile(args.app) as archive:
            archive.extractall(temporary)
        apps = list(Path(temporary).glob("Payload/*.app"))
        assert len(apps) == 1
        run(sim + ["terminate", device, PACKAGE], check=False)
        run(sim + ["install", device, apps[0]])
    container = Path(run(sim + ["get_app_container", device, PACKAGE, "data"]).stdout.strip())
    result_file = container / "Documents/pal-http-result.json"
    result_file.unlink(missing_ok=True)
    fixture_file = container / "Documents/fixture.json"
    fixture_file.write_text(json.dumps(fixture))
    # Simulator redirects these files inside its own data directory.
    stdout = container / "Documents/pal-http.stdout"
    stderr = container / "Documents/pal-http.stderr"
    run(sim + ["launch", f"--stdout={stdout}", f"--stderr={stderr}", device, PACKAGE])
    try:
        result = wait_report(lambda: result_file.read_text() if result_file.exists() else "", args.timeout)
    finally:
        run(sim + ["terminate", device, PACKAGE], check=False)
        fixture_file.unlink(missing_ok=True)
        for source in (stdout, stderr):
            if source.exists():
                (output / source.name).write_bytes(source.read_bytes())
    # Device/runtime identity comes from simctl, not an assumed host OS version.
    devices = json.loads(run(sim + ["list", "devices", "--json"]).stdout)["devices"]
    identity = next({"runtime": runtime, "device": value} for runtime, values in devices.items()
                    for value in values if value["udid"] == device)
    return result, identity


def android(args, output, fixture):
    serial = os.environ.get("H2_ANDROID_SERIAL")
    if not serial or not serial.startswith("emulator-"):
        raise ValueError("H2_ANDROID_SERIAL must explicitly identify the test emulator")
    sdk = os.environ.get("ANDROID_HOME")
    adb = [str(Path(sdk) / "platform-tools/adb") if sdk else "adb", "-s", serial]
    run(adb + ["shell", "am", "force-stop", PACKAGE])
    run(adb + ["install", "-r", args.app])
    run(adb + ["shell", "run-as", PACKAGE, "rm", "-f", "files/pal-http-result.json"])
    run(adb + ["shell", "run-as", PACKAGE, "mkdir", "-p", "files"])
    run(adb + ["shell", "run-as", PACKAGE, "tee", "files/fixture.json"], input=json.dumps(fixture))
    run(adb + ["shell", "am", "start", "-W", "-n", PACKAGE + "/.MainActivity"])
    try:
        result = wait_report(lambda: run(adb + ["shell", "run-as", PACKAGE, "cat",
                                                 "files/pal-http-result.json"], check=False).stdout, args.timeout)
    finally:
        log = run(adb + ["shell", "run-as", PACKAGE, "cat", "files/pal-http-result.json.log"], check=False)
        (output / "pal-http.stdout").write_text(log.stdout)
        pid = run(adb + ["shell", "pidof", PACKAGE], check=False).stdout.strip()
        if pid.isdigit():
            (output / "logcat.txt").write_text(run(adb + ["logcat", "-d", "--pid=" + pid]).stdout)
        run(adb + ["shell", "am", "force-stop", PACKAGE])
    run(adb + ["shell", "run-as", PACKAGE, "rm", "-f", "files/fixture.json"])
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
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-http-mobile-result")))
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    advertised = "127.0.0.1" if args.platform == "ios" else "10.0.2.2"
    with tempfile.TemporaryDirectory(prefix="pal-http-fixture-") as directory, Fixture(directory, advertised=advertised) as fixture:
        settings = dict(http=fixture.http, https=fixture.https, untrusted=fixture.untrusted,
                        ca=fixture.ca.read_text())
        result, environment = (ios if args.platform == "ios" else android)(args, args.output, settings)
        environment["fixture_attempts"] = fixture.verify_arrivals()
        environment["tls_rejection"] = fixture.verify_tls_rejection()
        environment["tls_verification"] = "required; explicit isolated test CA; separate untrusted certificate rejected"
    (args.output / "qualified.json").write_text(json.dumps(result, indent=2) + "\n")
    environment.update(observation_finished_at_utc=datetime.now(timezone.utc).isoformat(),
                       evidence_timezone="UTC", app_sha256=hashlib.sha256(args.app.read_bytes()).hexdigest(),
                       sdk_sha256=hashlib.sha256(args.sdk.read_bytes()).hexdigest())
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    verify(result, args.registry, "ios-simulator" if args.platform == "ios" else "android-emulator")
    print(f"PAL HTTP {args.platform}: 45/45 PASS, blocked=0, teardown=0")

if __name__ == "__main__":
    main()
