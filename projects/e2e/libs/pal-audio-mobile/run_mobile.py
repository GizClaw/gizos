#!/usr/bin/env python3
"""Install the packaged Audio PAL App and verify its mandatory case ledger."""

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

PACKAGE = "com.haivivi.gizos.e2e.palaudio"


def run(argv, check=True, timeout=45):
    result = subprocess.run([str(arg) for arg in argv], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(f"{argv}: {result.stdout}")
    return result


def wait_report(read, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return json.loads(read())
        except (ValueError, TypeError):
            time.sleep(0.25)
    raise TimeoutError(f"No complete Audio report after {timeout}s")


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_AUDIO_CASE\(\w+, "([^"]+)"\)', registry.read_text())
    assert len(ids) == len(set(ids)) == 24, "unexpected Audio registry"
    assert report["platform"] == platform
    assert report["image_version"] == "2.0.0"
    assert [item["id"] for item in report["cases"]] == ids
    assert all(item["status"] == "PASS" and item["detail"] == 0
               for item in report["cases"])
    for key, value in {"passed": 24, "failed": 0, "blocked": 0,
                       "run_rc": 0, "teardown_rc": 0}.items():
        assert report[key] == value, (key, report)
    assert report["mic_frames"] >= 2 and report["speaker_frames"] >= 2
    assert report["stability_elapsed_ms"] >= 30000
    assert report["output_peak"] > 0


def ios(args, output):
    device = os.environ.get("H2_IOS_SIMULATOR_UDID")
    if not device:
        raise ValueError("H2_IOS_SIMULATOR_UDID must identify a booted simulator")
    sim = ["xcrun", "simctl"]
    with tempfile.TemporaryDirectory(prefix="pal-audio-ios-") as temporary:
        with zipfile.ZipFile(args.app) as archive:
            archive.extractall(temporary)
        apps = list(Path(temporary).glob("Payload/*.app"))
        assert len(apps) == 1
        run(sim + ["terminate", device, PACKAGE], check=False)
        run(sim + ["install", device, apps[0]])
    container = Path(run(sim + ["get_app_container", device, PACKAGE, "data"]).stdout.strip())
    result_file = container / "Documents/pal-audio-result.json"
    result_file.unlink(missing_ok=True)
    stdout = container / "Documents/pal-audio.stdout"
    stderr = container / "Documents/pal-audio.stderr"
    run(sim + ["privacy", device, "grant", "microphone", PACKAGE])
    run(sim + ["launch", f"--stdout={stdout}", f"--stderr={stderr}", device, PACKAGE])
    try:
        report = wait_report(lambda: result_file.read_text()
                             if result_file.exists() else "", args.timeout)
    finally:
        run(sim + ["terminate", device, PACKAGE], check=False)
        for source in (stdout, stderr):
            if source.exists():
                (output / source.name).write_bytes(source.read_bytes())
    devices = json.loads(run(sim + ["list", "devices", "--json"]).stdout)["devices"]
    identity = next({"runtime": runtime, "device": value} for runtime, values in devices.items()
                    for value in values if value["udid"] == device)
    return report, identity


def android(args, output):
    serial = os.environ.get("H2_ANDROID_SERIAL")
    if not serial or not serial.startswith("emulator-"):
        raise ValueError("H2_ANDROID_SERIAL must identify the test emulator")
    sdk = os.environ.get("ANDROID_HOME")
    adb = [str(Path(sdk) / "platform-tools/adb") if sdk else "adb", "-s", serial]
    run(adb + ["shell", "am", "force-stop", PACKAGE], check=False)
    run(adb + ["install", "-r", args.app])
    run(adb + ["shell", "pm", "grant", PACKAGE, "android.permission.RECORD_AUDIO"])
    run(adb + ["shell", "run-as", PACKAGE, "rm", "-f", "files/pal-audio-result.json"])
    run(adb + ["shell", "am", "start", "-W", "-n", PACKAGE + "/.MainActivity"])
    try:
        report = wait_report(lambda: run(adb + ["shell", "run-as", PACKAGE, "cat",
                                           "files/pal-audio-result.json"], check=False).stdout,
                             args.timeout)
    finally:
        pid = run(adb + ["shell", "pidof", PACKAGE], check=False).stdout.strip()
        if pid.isdigit():
            (output / "logcat.txt").write_text(run(adb + ["logcat", "-d", "--pid=" + pid]).stdout)
        run(adb + ["shell", "am", "force-stop", PACKAGE], check=False)
    with zipfile.ZipFile(args.app) as app, zipfile.ZipFile(args.sdk) as aar:
        assert app.read("lib/arm64-v8a/libh2_pal_core.so") == \
            aar.read("jni/arm64-v8a/libh2_pal_core.so"), "APK did not use AAR binary"
    return report, {"serial": serial,
                    "api": run(adb + ["shell", "getprop", "ro.build.version.sdk"]).stdout.strip(),
                    "fingerprint": run(adb + ["shell", "getprop", "ro.build.fingerprint"]).stdout.strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR",
                                                    "/tmp/pal-audio-mobile-result")))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report, environment = (ios if args.platform == "ios" else android)(args, args.output)
    (args.output / "qualified.json").write_text(json.dumps(report, indent=2) + "\n")
    environment.update(observation_finished_at_utc=datetime.now(timezone.utc).isoformat(),
                       evidence_timezone="UTC", app_sha256=hashlib.sha256(args.app.read_bytes()).hexdigest(),
                       sdk_sha256=hashlib.sha256(args.sdk.read_bytes()).hexdigest())
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    verify(report, args.registry,
           "ios-simulator" if args.platform == "ios" else "android-emulator")
    print(f"PAL Audio {args.platform}: 24/24 PASS blocked=0")


if __name__ == "__main__":
    main()
