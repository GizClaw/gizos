"""Reject incomplete, stale or driver-only six-platform Display qualification."""
from pathlib import Path
import hashlib
import json
import re


ROOT = Path("projects/e2e/apps/pal-display")
IDS = re.findall(r'H2_PAL_DISPLAY_CASE\("([^"]+)"',
                 (ROOT / "app/include/h2_pal_display_cases.inc").read_text())


def report(value, cases):
    assert [case["id"] for case in cases] == IDS
    assert all(case["status"] == "PASS" and case["rc"] == 0 for case in cases)
    for key, expected in {
        "contract": 1, "operations": 6, "passed": len(IDS), "failed": 0,
        "not_run": 0, "observations": 23, "complete": 1, "qualified": 1,
        "rc": 0, "cleanup": 0, "teardown": 0,
    }.items():
        assert value[key] == expected, (key, value[key])


def board(value, build):
    assert value["driver_qualified"] and value["panel_qualified"]
    assert value["optical_verified"] is False
    physical = value["physical_observation"]
    assert physical["status"] == "PASS" and physical["source"] == "user"
    assert physical["pattern_observed"] and physical["stable_image_observed"]
    assert physical["image_version"] == value["version"]
    assert physical["device_uid"] == value["final_status"]["device_uid"]
    assert physical["image_sha256"] == value["final_status"]["active_checksum"]
    # Human pattern confirmation does not establish measured luminance or an
    # observation of every dimming step. Keep that evidence boundary explicit.
    assert value["brightness_validation"]["mandatory_cases"] == "PASS"
    assert value["brightness_validation"]["optical_measurement"] == "NOT_MEASURED"
    boots = [value["install_boot"], value["independent_normal_reboot"]]
    assert boots[0]["log_sha256"] != boots[1]["log_sha256"]
    for boot in boots:
        assert boot["fresh_boot"] and boot["confirmed"]
        assert boot["version"] == value["version"]
        report(boot["qualified"], boot["cases"])
        received = boot["real_driver_observations_received"]
        assert 0 < len(received) <= 23
        assert all("rc=0" in observation for observation in received)
        assert boot["received_observations"] == len(received)
        if len(received) < 23:
            assert boot["transport_limitation"]
    before, final = value["before_status"], value["final_status"]
    for key in ["device_uid", "partition_1_package_checksum",
                "partition_1_image_checksum"]:
        assert before[key] == final[key]
    for key, expected in {"active_role": "app", "stage_valid": "0",
                          "running_partition": "2", "next_partition": "2",
                          "last_result": "0"}.items():
        assert final[key] == expected
    manifest = build["package_manifest"]
    assert final["active_version"] == value["version"] == manifest["version"]
    assert final["active_checksum"] == manifest["image_sha256"]
    assert final["partition_2_package_checksum"] == value["package_sha256"]
    assert build["assets"][0]["sha256"] == value["package_sha256"]
    assert value["p1_preserved"] and value["stage_empty"]
    assert value["running_and_next_app"]
    if value["board"] == "amoled":
        assert "blank=1" in value["coredump_status"]
    else:
        assert value["coredump_before_sha256"] == value["coredump_after_sha256"]
        assert "stored_bytes=32" in value["coredump_status"]


def mobile(platform, environment):
    for key in ["app_sha256", "sdk_sha256"]:
        assert re.fullmatch(r"[0-9a-f]{64}", environment[key]), key
    assert environment["evidence_timezone"] == "UTC"
    assert environment["observation_finished_at_utc"]
    if platform == "android":
        assert environment["apk_aar_binary_identical"]
        assert environment["aar_binary_sha256"] == environment["apk_binary_sha256"]
        assert "h2_android_platform_display_api" in environment["public_provider_symbols"]
    else:
        assert environment["sdk_binary_member"].startswith(
            "H2PALCore.xcframework/ios-arm64-simulator/")
        assert "_h2_ios_platform_display_api" in environment["provider_symbols_in_sdk_and_ipa"]
        assert environment["device"]["isAvailable"]


def main():
    assert len(IDS) == 24 and len(set(IDS)) == 24
    value = json.loads((ROOT / "qualification.json").read_text())
    assert value["qualified"], "Display qualification remains incomplete"
    assert value["driver_and_ui_qualified"] and value["human_observation_verified"]
    assert value["optical_measurement_automated"] is False
    assert not value["pending"]
    assert {item["platform"] for item in value["platforms"]} == {
        "macos", "wasm", "ios", "android", "amoled", "bk7258"}
    assert len(value["platforms"]) == 6
    for path, expected in value["source_sha256"].items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected, path
    for item in value["platforms"]:
        assert item["status"] == "PASS", item["platform"]
        path = Path(item["evidence"])
        receipt = json.loads(path.read_text())
        if item["platform"] in {"amoled", "bk7258"}:
            board(receipt, json.loads(path.with_name("build.json").read_text()))
        else:
            report(receipt, receipt["cases"])
            if item["platform"] in {"ios", "android"}:
                mobile(item["platform"], json.loads(
                    path.with_name("environment.json").read_text()))
    coverage = json.loads(Path(
        "projects/e2e/targets/cc_binary/pal-display/evidence/coverage.json").read_text())
    for path in ["h2_pal_display_e2e.c", "h2_sdl3_display.cpp"]:
        assert coverage[path]["functions"]["percent"] > 50
        assert coverage[path]["lines"]["percent"] > 50
    print("PAL Display: six platforms qualified; manual optical evidence remains distinct")


if __name__ == "__main__":
    main()
