"""Run two independent storage processes and prove persistence with one nonce."""
import json
import re
import secrets
import shutil


def verify(phases, registry, platform, nonce, contract):
    expected = {name: int(phase) for name, phase in re.findall(contract["registry_pattern"], registry.read_text())}
    assert len(expected) == contract["case_count"] and len(phases) == 2
    assert phases[0]["pid"] != phases[1]["pid"]
    cases = []
    for phase, result in enumerate(phases, 1):
        selected = [key for key, value in expected.items() if value == phase]
        assert result["phase"] == phase and result["nonce"] == nonce
        assert len(result["cases"]) == len(selected) and [r["id"] for r in result["cases"]] == selected
        assert all(r["status"] == "PASS" and r[contract["case_result"]] == 0 and r["phase"] == phase and r["nonce"] == nonce for r in result["cases"])
        assert result["passed"] == len(selected)
        for key, value in contract["options"]["phase_expected"].items():
            assert result[key] == value, (key, result)
        cases.extend(result["cases"])
    return dict(contract["expected"], platform=platform, passed=len(cases), cases=cases, phases=phases)


def run_suite(app, args):
    nonce = secrets.randbits(args.contract["options"]["nonce_bits"])
    root = args.contract["options"]["root"]
    if app.platform == "ios":
        test_root = app.container / "Documents" / root
        if test_root.exists():
            shutil.rmtree(test_root)
    else:
        app.adb_command("shell", "run-as", app.package, "rm", "-rf", "files/" + root)
    phases = []
    for phase in (1, 2):
        phases.append(app.launch(
            ios_args=("--phase", str(phase), "--nonce", str(nonce)),
            android_args=("--ei", "phase", str(phase), "--el", "nonce", str(nonce)),
        ))
    if app.platform == "ios":
        assert not (test_root / "files/run").exists()
    else:
        assert app.adb_command("shell", "run-as", app.package, "test", "-d",
                               "files/" + root + "/files/run", check=False).returncode != 0
    (args.output / "phases.json").write_text(json.dumps(phases, indent=2) + "\n")
    return verify(phases, args.registry, args.platform, nonce, args.contract)
