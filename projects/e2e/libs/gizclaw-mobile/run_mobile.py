#!/usr/bin/env python3
"""Run the packaged GizClaw App against an explicitly configured E2E service."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).absolute().parents[4] / "projects/e2e/apps/gizclaw"))
import api_coverage

PACKAGE = "com.haivivi.gizos.e2e.gizclaw"


def run(argv, *, check=True, data=None):
    result = subprocess.run([str(a) for a in argv], input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
    if check and result.returncode:
        # Inputs can include a token; never echo stdin or unrestricted output.
        raise RuntimeError(f"command failed ({result.returncode}): {argv[0]}")
    return result


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("ios", "android"))
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--pcm", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    endpoint = os.environ.get("H2_GIZCLAW_E2E_ENDPOINT", "")
    token = os.environ.get("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", "")
    api = os.environ.get("H2_GIZCLAW_E2E_DEVICE_API_URL", "")
    audio = os.environ.get("H2_GIZCLAW_E2E_AUDIO_URL", "")
    if not re.fullmatch(r"[a-zA-Z0-9.-]+:[0-9]{1,5}", endpoint) or not 0 < int(endpoint.rsplit(":", 1)[1]) <= 65535:
        parser.error("explicit hostname:port endpoint required")
    if not token or len(token) > 4096 or not api.startswith("https://") or not audio.startswith("https://"):
        parser.error("explicit E2E token and HTTPS device API/audio fixture URLs required")
    fixture = json.dumps(dict(endpoint=endpoint, token=token, api_url=api, audio_url=audio)).encode()
    args.output.mkdir(parents=True, exist_ok=True)
    app_copy = args.output / ("app.ipa" if args.platform == "ios" else "app.apk")
    sdk_copy = args.output / "sdk.archive"
    shutil.copyfile(args.app, app_copy)
    shutil.copyfile(args.sdk, sdk_copy)
    args.app, args.sdk = app_copy, sdk_copy
    identity = {}
    if args.platform == "ios":
        device = os.environ.get("H2_IOS_SIMULATOR_UDID")
        if not device:
            parser.error("H2_IOS_SIMULATOR_UDID must select the owned simulator")
        sim = ["xcrun", "simctl"]
        run(sim + ["terminate", device, PACKAGE], check=False)
        with tempfile.TemporaryDirectory(prefix="gizclaw-ios-") as tmp:
            with zipfile.ZipFile(args.app) as archive:
                archive.extractall(tmp)
            apps = list(Path(tmp).glob("Payload/*.app"))
            if len(apps) != 1:
                raise ValueError("expected one application")
            run(sim + ["install", device, apps[0]])
        container = Path(run(sim + ["get_app_container", device, PACKAGE, "data"]).stdout.decode().strip()) / "Documents"
        container.mkdir(exist_ok=True)
        fixture_file = container / "fixture.json"
        descriptor = os.open(fixture_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(fixture)
        shutil.copyfile(args.pcm, container / "voice.pcm")
        for filename in ("gizclaw-result.json", "gizclaw.log"):
            (container / filename).unlink(missing_ok=True)
        run(sim + ["launch", device, PACKAGE])
        def read(filename):
            p = container / filename
            return p.read_bytes() if p.exists() else b""
        def clean():
            fixture_file.unlink(missing_ok=True)
        def stop():
            run(sim + ["terminate", device, PACKAGE], check=False)
        devices = json.loads(run(sim + ["list", "devices", "--json"]).stdout)["devices"]
        identity = next({"runtime": runtime, "device": d} for runtime, ds in devices.items()
                        for d in ds if d["udid"] == device)
    else:
        serial = os.environ.get("H2_ANDROID_SERIAL", "")
        if not serial.startswith("emulator-"):
            parser.error("H2_ANDROID_SERIAL must select the owned emulator")
        sdk = Path(os.environ.get("ANDROID_HOME", "/nonexistent"))
        adb = [sdk / "platform-tools/adb", "-s", serial]
        run(adb + ["shell", "am", "force-stop", PACKAGE])
        run(adb + ["install", "-r", args.app])
        base = adb + ["shell", "run-as", PACKAGE]
        run(base + ["mkdir", "-p", "files"])
        run(base + ["touch", "files/fixture.json"])
        run(base + ["chmod", "600", "files/fixture.json"])
        run(base + ["tee", "files/fixture.json"], data=fixture)
        run(base + ["tee", "files/voice.pcm"], data=args.pcm.read_bytes())
        run(base + ["rm", "-f", "files/gizclaw-result.json", "files/gizclaw.log"])
        run(adb + ["shell", "am", "start", "-n", PACKAGE + "/.MainActivity"])
        def read(filename):
            return run(base + ["cat", "files/" + filename], check=False).stdout
        def clean():
            run(base + ["rm", "-f", "files/fixture.json"], check=False)
        def stop():
            run(adb + ["shell", "am", "force-stop", PACKAGE], check=False)
        identity = {"serial": serial, "fingerprint": run(adb + ["shell", "getprop", "ro.build.fingerprint"]).stdout.decode().strip()}
    result = None
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            try:
                result = json.loads(read("gizclaw-result.json"))
                break
            except (ValueError, TypeError):
                time.sleep(1)
        log = read("gizclaw.log")
        leaked = token.encode() in log
        if leaked:
            log = log.replace(token.encode(), b"[REDACTED]")
        (args.output / "test.log").write_bytes(log)
        if result is None:
            raise TimeoutError("No terminal result; App retained for investigation")
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        expected_platform = "ios-simulator" if args.platform == "ios" else "android-emulator"
        expected = dict(platform=expected_platform, rc=0, teardown=0, selected=8,
                        passed=8, failed=0, cleanup_rc=0, retained_resources=0, complete=True)
        passed = all(result.get(k) == v for k, v in expected.items()) and not leaked
        audit = api_coverage.audit(log.decode("utf-8").splitlines(keepends=True),
            api_coverage.requirements(), endpoint=endpoint, backend="h2peer",
            profile=args.profile, platform=expected_platform,
            process_exit_code=0 if passed else 1)
        (args.output / "coverage.json").write_text(json.dumps(audit, indent=2) + "\n")
        evidence = dict(qualified=passed and audit["valid"], all_cases_passed=passed,
                        api_covered=audit["covered"], identity=identity,
                        app_sha256=sha(args.app), sdk_sha256=sha(args.sdk),
                        pcm_sha256=sha(args.pcm), log_sha256=hashlib.sha256(log).hexdigest(),
                        redaction_required=leaked)
        (args.output / "environment.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps({"platform": expected_platform, "all_cases_passed": passed,
                          "passed": result.get("passed"), "cleanup_rc": result.get("cleanup_rc"),
                          "retained_resources": result.get("retained_resources")}))
        return 0 if passed and audit["valid"] else 1
    finally:
        clean()
        if result is not None and result.get("retained_resources") == 0:
            stop()


if __name__ == "__main__":
    raise SystemExit(main())
