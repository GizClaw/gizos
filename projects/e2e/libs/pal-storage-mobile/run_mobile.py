"""Run both storage phases in separate native App processes, using one sandbox."""
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import shutil

from tools.bazel.mobile_e2e import MobileApp, save_evidence

PACKAGE = "com.haivivi.gizos.e2e.palstorage"


def verify(phases, registry, platform, nonce):
    expected = {name: int(phase) for name, phase in re.findall(r'H2_PAL_STORAGE_CASE\("([^"]+)", ([12])\)', registry.read_text())}
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
    output = {"platform": platform, "contract": 1, "operations": 28, "qualified": True, "passed": 30, "failed": 0, "blocked": 0, "cases": cases, "phases": phases}
    return output


def run_suite(app, nonce):
    if app.platform == "ios":
        test_root = app.container / "Documents/pal-storage"
        if test_root.exists():
            shutil.rmtree(test_root)
    else:
        app.adb_command("shell", "run-as", PACKAGE, "rm", "-rf", "files/pal-storage")
    phases = []
    for phase in (1, 2):
        phases.append(app.launch(
            ios_args=("--phase", str(phase), "--nonce", str(nonce)),
            android_args=("--ei", "phase", str(phase), "--el", "nonce", str(nonce)),
        ))
    if app.platform == "ios":
        assert not (test_root / "files/run").exists()
    else:
        assert app.adb_command("shell", "run-as", PACKAGE, "test", "-d",
                               "files/pal-storage/files/run", check=False).returncode != 0
    return phases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path); parser.add_argument("sdk", type=Path); parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-storage-mobile")))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    nonce = secrets.randbits(32)
    with MobileApp(args.platform, args.app, args.sdk, PACKAGE,
                   "pal-storage-phase.json", args.output,
                   timeout=args.timeout, prefix="pal-storage") as app:
        phases = run_suite(app, nonce)
        (args.output / "phases.json").write_text(json.dumps(phases, indent=2) + "\n")
        report = verify(phases, args.registry, args.platform, nonce)
        save_evidence(args.output, report, app.environment(), args.app, args.sdk)
    print(f"PAL Storage {args.platform}: operations=28, 30/30 PASS, restart=PASS, cleanup=0, qualified=1")


if __name__ == "__main__":
    main()
