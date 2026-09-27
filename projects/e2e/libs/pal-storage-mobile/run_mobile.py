"""Run both storage phases in separate native App processes, using one sandbox."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import tempfile
import time
import zipfile

PACKAGE = "com.haivivi.gizos.e2e.palstorage"

def run(argv, check=True):
    result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, timeout=45)
    if check and result.returncode: raise RuntimeError(f"{argv}: {result.stdout}{result.stderr}")
    return result

def wait_report(read):
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        try: return json.loads(read())
        except (ValueError, FileNotFoundError): time.sleep(0.25)
    raise TimeoutError("storage phase exceeded 90s")

def ios(args, nonce):
    device = os.environ["H2_IOS_SIMULATOR_UDID"]
    sim = ["xcrun", "simctl"]
    run(sim + ["terminate", device, PACKAGE], check=False)
    with tempfile.TemporaryDirectory(prefix="storage-ipa-") as directory:
        with zipfile.ZipFile(args.app) as archive: archive.extractall(directory)
        app, = Path(directory).glob("Payload/*.app")
        run(sim + ["install", device, app])
    container = Path(run(sim + ["get_app_container", device, PACKAGE, "data"]).stdout.strip())
    test_root = container / "Documents/pal-storage"
    if test_root.exists(): shutil.rmtree(test_root)
    report = container / "Documents/pal-storage-phase.json"
    phases = []
    for phase in (1, 2):
        report.unlink(missing_ok=True)
        run(sim + ["launch", device, PACKAGE, "--phase", str(phase), "--nonce", str(nonce)])
        try: phases.append(wait_report(report.read_text))
        finally: run(sim + ["terminate", device, PACKAGE], check=False)
    assert not (test_root / "files/run").exists()
    devices = json.loads(run(sim + ["list", "devices", "--json"]).stdout)["devices"]
    identity = next({"runtime": runtime, "device": value} for runtime, values in devices.items() for value in values if value["udid"] == device)
    return phases, identity

def android(args, nonce):
    serial = os.environ["H2_ANDROID_SERIAL"]
    if not serial.startswith("emulator-"): raise ValueError("explicit test emulator required")
    adb = [str(Path(os.environ["ANDROID_HOME"]) / "platform-tools/adb"), "-s", serial]
    run(adb + ["shell", "am", "force-stop", PACKAGE])
    run(adb + ["install", "-r", args.app])
    run(adb + ["shell", "run-as", PACKAGE, "rm", "-rf", "files/pal-storage"])
    phases = []
    for phase in (1, 2):
        run(adb + ["shell", "run-as", PACKAGE, "rm", "-f", "files/pal-storage-phase.json"])
        run(adb + ["shell", "am", "start", "-W", "-n", PACKAGE + "/.MainActivity", "--ei", "phase", phase, "--el", "nonce", nonce])
        try: phases.append(wait_report(lambda: run(adb + ["shell", "run-as", PACKAGE, "cat", "files/pal-storage-phase.json"], check=False).stdout))
        finally: run(adb + ["shell", "am", "force-stop", PACKAGE])
    assert run(adb + ["shell", "run-as", PACKAGE, "test", "-d", "files/pal-storage/files/run"], check=False).returncode != 0
    with zipfile.ZipFile(args.app) as app, zipfile.ZipFile(args.sdk) as sdk:
        assert app.read("lib/arm64-v8a/libh2_pal_core.so") == sdk.read("jni/arm64-v8a/libh2_pal_core.so")
    return phases, {"serial": serial, "api": run(adb + ["shell", "getprop", "ro.build.version.sdk"]).stdout.strip()}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path); parser.add_argument("sdk", type=Path); parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-storage-mobile")))
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    nonce = secrets.randbits(32)
    phases, environment = (ios if args.platform == "ios" else android)(args, nonce)
    (args.output / "phases.json").write_text(json.dumps(phases, indent=2) + "\n")
    expected = {name: int(phase) for name, phase in re.findall(r'H2_PAL_STORAGE_CASE\("([^"]+)", ([12])\)', args.registry.read_text())}
    assert len(expected) == 30 and len(phases) == 2
    assert phases[0]["pid"] != phases[1]["pid"]
    cases = []
    for phase, result in enumerate(phases, 1):
        selected = {key for key, value in expected.items() if value == phase}
        assert result["contract"] == 1 and result["phase"] == phase and result["nonce"] == nonce
        assert len(result["cases"]) == len(selected) and {r["id"] for r in result["cases"]} == selected
        assert all(r["status"] == "PASS" and r["rc"] == 0 and r["phase"] == phase and r["nonce"] == nonce for r in result["cases"])
        assert result["passed"] == len(selected)
        assert result["failed"] == result["blocked"] == result["cleanup"] == result["teardown"] == result["rc"] == 0
        cases.extend(result["cases"])
    output = {"platform": args.platform, "contract": 1, "operations": 28, "qualified": True, "passed": 30, "failed": 0, "blocked": 0, "cases": cases, "phases": phases}
    (args.output / "qualified.json").write_text(json.dumps(output, indent=2) + "\n")
    environment.update(observed_at_utc=datetime.now(timezone.utc).isoformat(), app_sha256=hashlib.sha256(args.app.read_bytes()).hexdigest(), sdk_sha256=hashlib.sha256(args.sdk.read_bytes()).hexdigest())
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    print(f"PAL Storage {args.platform}: operations=28, 30/30 PASS, restart=PASS, cleanup=0, qualified=1")

if __name__ == "__main__": main()
