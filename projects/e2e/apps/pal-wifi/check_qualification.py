"""Check recorded qualification; --audit-artifacts also verifies local raw bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from evidence_validator import verify_boot
from fixture_validator import verify_fixture
from receipt_bindings import check_captured_status, check_status

BASE = Path("projects/e2e/apps/pal-wifi")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check(audit_artifacts=False):
    manifest = json.loads((BASE / "qualification.json").read_text(encoding="utf-8"))
    assert manifest["qualified"] is True and manifest["pending"] == []
    assert manifest["operation_count"] == 21 and manifest["case_count"] == 39
    for path, expected in manifest["source_inputs"].items():
        assert digest(path) == expected, f"untested source changed: {path}"
    ids = re.findall(r'H2_WIFI_CASE\("([^"]+)"', (BASE / "app/include/h2_pal_wifi_cases.inc").read_text())
    assert len(ids) == 38 and len(set(ids)) == 38
    platforms = manifest["platforms"]
    assert {p["platform"] for p in platforms} == {"macos", "wasm", "ios", "android", "devkit", "bk7258"}
    for platform in platforms:
        file = Path(platform["evidence"])
        assert digest(file) == platform["sha256"]
        report = json.loads(file.read_text(encoding="utf-8"))
        if platform["platform"] not in {"devkit", "bk7258"}:
            assert report["capability_contract_pass"] == 1 and report["physical_wifi_qualified"] == 0
            assert report["operations"] == 21 and report["rc"] == 0
            if platform["platform"] in {"ios", "android", "wasm"}:
                assert report["teardown"] == 0
            if platform["platform"] == "wasm":
                assert report["worker"] == 1 and report["offline_event"] == 1 and report["online_event"] == 1
            continue
        assert report["qualified"] is True and report["case_count"] == 39
        check_captured_status(report)
        if audit_artifacts:
            check_status(report)
        runs = report["runs"]
        assert len(runs) >= 3
        verified = []
        for index, run in enumerate(runs):
            assert digest(run["log"]) == run["sha256"]
            expected_version = run["version"] if index == 0 else report["version"]
            verified.append(verify_boot(run["log"], ids, platform["platform"], expected_version, seed=index == 0))
        assert len({(r["boot"]["boot"], r["boot"]["nonce"]) for r in verified}) == len(runs)
        assert report["p1_unchanged"] is True and report["stage_empty"] is True and report["coredump_unchanged"] is True
        assert len(report["package_sha256"]) == 64 and len(report["image_sha256"]) == 64
        assert digest(report["fixture"]) == report["fixture_sha256"]
        fixture = json.loads(Path(report["fixture"]).read_text(encoding="utf-8"))
        assert fixture["cleanup"] == 0 and fixture["settings_unchanged"] == 1 and fixture["network_restored"] == 1
        assert fixture["p1_unchanged"] is True and fixture["coredump_unchanged"] is True and fixture["stage_empty"] is True
        observed = verify_fixture(fixture, audit_artifacts)
        for row, run in zip(runs, verified):
            c = run["report"]["CLIENT"]
            assert row["fixture_log"] in observed, "missing associated fixture window"
            assert (report["fixture_target"], c["mac"], c["ip4"]) in observed[row["fixture_log"]], \
                "DUT lease lacks real-peer corroboration in its associated fixture window"
    scope = "local raw artifact audit" if audit_artifacts else "historical receipt/source consistency"
    print("PAL_WIFI_GATE_READY: two recorded physical WLAN qualifications and four host capability rows; " + scope)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit-artifacts", action="store_true", help="require retained local packages, status logs and dumps")
    check(parser.parse_args().audit_artifacts)
