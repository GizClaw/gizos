"""Reject incomplete, stale or driver-only six-platform Display qualification."""
from pathlib import Path
import hashlib
import json
import re


ROOT = Path("projects/e2e/apps/pal-display")
IDS = re.findall(r'H2_PAL_DISPLAY_CASE\("([^"]+)"',
                 (ROOT / "app/include/h2_pal_display_cases.inc").read_text(encoding="utf-8"))


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


# Only host harness/audit files can supersede historical source receipts here.
# Production provider, App, registry and board receipts remain mandatory.
RUNNER_SOURCES = {
    "projects/e2e/apps/pal-display/BUILD.bazel",
    "projects/e2e/apps/pal-display/check_qualification.py",
    "projects/e2e/apps/pal-display/README.md",
    "projects/e2e/libs/pal-display-mobile/run_mobile.py",
    "projects/e2e/targets/android_binary/pal-display/BUILD.bazel",
    "projects/e2e/targets/ios_application/pal-display/BUILD.bazel",
    "tools/bazel/mobile_e2e.py",
    "tools/bazel/mobile_e2e.bzl",
}
REMOVED_RUNNERS = {
    "projects/e2e/targets/android_binary/pal-display/run_simulator.sh",
    "projects/e2e/targets/ios_application/pal-display/run_simulator.sh",
}


def runner_refactor(historical):
    """Check new mobile observations without relabeling old board evidence."""
    followup = json.loads((ROOT / "mobile_runner_refactor.json").read_text(encoding="utf-8"))
    assert followup["schema"] == 1
    assert followup["new_physical_run_claimed"] is False
    assert re.fullmatch(r"[0-9a-f]{40}", followup["mobile_execution_commit"])
    assert hashlib.sha256((ROOT / "qualification.json").read_bytes()).hexdigest() == followup["historical_qualification_sha256"]
    current = followup["current_source_sha256"]
    assert set(current) == RUNNER_SOURCES
    assert set(followup["removed_source_sha256"]) == REMOVED_RUNNERS
    for path, expected in followup["removed_source_sha256"].items():
        assert historical[path] == expected and not Path(path).exists(), path
    for path, expected in {**historical, **current}.items():
        if path not in REMOVED_RUNNERS:
            assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected, path
    # These exact Python sources and consumer declarations executed the stored runs.
    executed = followup["executed_runner_sha256"]
    assert set(executed) == {
        "tools/bazel/mobile_e2e.py",
        "projects/e2e/libs/pal-display-mobile/run_mobile.py",
        "projects/e2e/targets/android_binary/pal-display/BUILD.bazel",
        "projects/e2e/targets/ios_application/pal-display/BUILD.bazel",
        "tools/bazel/mobile_e2e.bzl",
    }
    assert all(current[path] == sha for path, sha in executed.items())
    assert set(followup["mobile_runs"]) == {"ios", "android"}
    for platform, receipt in followup["mobile_runs"].items():
        result, environment = receipt["qualified"], receipt["environment"]
        report(result, result["cases"])
        assert result["platform"] == ("ios-simulator" if platform == "ios" else "android-emulator")
        mobile(platform, environment)
        assert environment["platform"] == platform and environment["runner_status"] == "completed"


def main():
    assert len(IDS) == 24 and len(set(IDS)) == 24
    value = json.loads((ROOT / "qualification.json").read_text(encoding="utf-8"))
    assert value["qualified"], "Display qualification remains incomplete"
    assert value["driver_and_ui_qualified"] and value["human_observation_verified"]
    assert value["optical_measurement_automated"] is False
    assert not value["pending"]
    assert {item["platform"] for item in value["platforms"]} == {
        "macos", "wasm", "ios", "android", "amoled", "bk7258"}
    assert len(value["platforms"]) == 6
    overrides = value.get("review_fix_source_sha256", {})
    runner_refactor({**value["source_sha256"], **overrides})
    if overrides:
        fix = value["review_fix_validation"]
        assert value["source_snapshot_commit"]
        assert fix["backlight_failure_regression"] == "PASS"
        assert fix["sdl_enum_sanitizer_regression"] == "PASS"
        desktop = json.loads(Path(fix["desktop_run_evidence"]).read_text(encoding="utf-8"))
        report(desktop, desktop["cases"])
        assert fix["new_physical_run_claimed"] is False
        build = json.loads(Path(fix["native_build_evidence"]).read_text(encoding="utf-8"))
        assert build["package_manifest"]["version"] == fix["native_build_version"]
        assert build["package_manifest"]["role"] == "app"
    for item in value["platforms"]:
        assert item["status"] == "PASS", item["platform"]
        path = Path(item["evidence"])
        receipt = json.loads(path.read_text(encoding="utf-8"))
        if item["platform"] in {"amoled", "bk7258"}:
            board(receipt, json.loads(path.with_name("build.json").read_text(encoding="utf-8")))
        else:
            report(receipt, receipt["cases"])
            if item["platform"] in {"ios", "android"}:
                mobile(item["platform"], json.loads(
                    path.with_name("environment.json").read_text(encoding="utf-8")))
    coverage = json.loads(Path(
        "projects/e2e/targets/cc_binary/pal-display/evidence/coverage.json").read_text(encoding="utf-8"))
    for path in ["h2_pal_display_e2e.c", "h2_sdl3_display.cpp"]:
        assert coverage[path]["functions"]["percent"] > 50
        assert coverage[path]["lines"]["percent"] > 50
    print("PAL Display: historical six-platform qualification and shared mobile runner evidence verified; no new physical run claimed")


if __name__ == "__main__":
    main()
