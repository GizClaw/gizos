"""Cross-check checked-in six-platform Audio receipts against the case registry.

Local development may pass --allow-pending to validate finished platforms while
BK device work is in progress. CI uses the default strict six-platform gate.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path("projects/e2e/apps/pal-audio")
EXPECTED_PLATFORMS = {
    "macos", "wasm_chromium", "ios_simulator", "android_emulator",
    "amoled_esp32s3", "bk7258",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def records(text, prefix):
    parsed = []
    for line in text.replace("\r", "\n").splitlines():
        if prefix not in line:
            continue
        try:
            parsed.append(json.loads(line.split(prefix, 1)[1]))
        except ValueError:
            # UART command frames can interrupt a log line. The device
            # replays its terminal ledger, so require a complete clean set.
            pass
    return parsed


def checked_file(item, field, hash_field):
    path = ROOT / item[field]
    assert path.is_file(), path
    assert digest(path) == item[hash_field], (field, path)
    return path


def check_summary(summary, expected, platform):
    for key, value in expected.items():
        assert summary.get(key) == value, (platform, key, value, summary.get(key))
    assert summary["passed"] == 24
    assert summary["failed"] == summary["blocked"] == 0
    assert summary["stability_elapsed_ms"] >= 30000
    assert summary["mic_frames"] >= 2 and summary["speaker_frames"] >= 2
    assert summary["output_peak"] > 0


def check_text(item, ids, platform, *, board=False):
    path = checked_file(item, "log", "log_sha256")
    text = path.read_text(errors="replace")
    if board:
        # A pre-reboot replay is not a fresh run. Only accept the ledger after
        # this capture's new App boot marker, and reject another App reboot.
        assert text.count("H2_PAL_AUDIO_BOOT ") == 1, platform
        text = text.split("H2_PAL_AUDIO_BOOT ", 1)[1]
    cases = records(text, "H2_PAL_AUDIO_CASE ")
    replay = records(text, "H2_PAL_AUDIO_REPLAY ")
    assert not [case for case in cases + replay
                if case.get("status") in ("FAIL", "BLOCKED")], platform
    assert set(ids) == {case["id"] for case in cases + replay
                        if case.get("status") == "PASS" and case.get("detail") == 0}, platform
    summaries = records(text, "H2_PAL_AUDIO_SUMMARY ")
    assert summaries, platform
    check_summary(summaries[-1], item["summary"], platform)
    if board:
        assert summaries[-1]["rc"] == summaries[-1]["confirm"] == 0
    if platform == "wasm_chromium":
        assert summaries[-1]["worker"] == 1 and summaries[-1]["mic_peak"] > 0
    if platform == "macos":
        assert summaries[-1]["witness_mic_frames"] > 0
        assert summaries[-1]["witness_speaker_frames"] > 0
        assert summaries[-1]["witness_speaker_peak"] > 0
    return text


def check_mobile(item, ids, platform):
    report = json.loads(checked_file(item, "qualified_report", "qualified_report_sha256").read_text())
    environment = json.loads(checked_file(item, "environment", "environment_sha256").read_text())
    assert [case["id"] for case in report["cases"]] == ids, platform
    assert all(case["status"] == "PASS" and case["detail"] == 0
               for case in report["cases"]), platform
    check_summary(report, item["summary"], platform)
    assert report["run_rc"] == report["teardown_rc"] == 0
    assert report["allocator_probe_passed"] == report["allocator_failure_rejected"] == 1
    assert report["allocator_allocations"] > 0
    assert report["allocator_allocations"] == report["allocator_frees"]
    assert environment["app_sha256"] == item["app_sha256"]
    assert environment["sdk_sha256"] == item["sdk_sha256"]


def status_fields(path):
    line = path.read_text().strip()
    assert line.startswith("H2_LOADER_STATUS "), path
    return dict(part.split("=", 1) for part in line.split()[1:] if "=" in part)


def check_board(item, ids, platform):
    check_text(item, ids, platform, board=True)
    boot_log = (ROOT / item["log"]).read_text(errors="replace")
    assert "H2_LOADER_REBOOT target=upgrade result=accepted" in boot_log
    assert boot_log.index("H2_LOADER_REBOOT target=upgrade result=accepted") < boot_log.index("H2_PAL_AUDIO_BOOT ")
    reboot = item["independent_reboot"]
    check_text(reboot, ids, platform + "_reboot", board=True)
    reboot_log = (ROOT / reboot["log"]).read_text(errors="replace")
    assert "H2_LOADER_REBOOT target=app result=accepted" in reboot_log
    assert reboot_log.index("H2_LOADER_REBOOT target=app result=accepted") < reboot_log.index("H2_PAL_AUDIO_BOOT ")
    status_path = ROOT / f"evidence/{'amoled' if platform == 'amoled_esp32s3' else 'bk7258'}/status.log"
    coredump_path = status_path.with_name("coredump.log")
    assert digest(status_path) == item["status_sha256"]
    assert digest(coredump_path) == item["coredump_sha256"]
    status = status_fields(status_path)
    for field, value in {
        "active_role": "app", "running_partition": "2", "next_partition": "2",
        "boot_intent": "auto", "stage_valid": "0", "partition_1_valid": "1",
        "partition_1_role": "loader", "partition_2_valid": "1",
        "partition_2_role": "app", "last_result": "0",
    }.items():
        assert status[field] == value, (platform, field, status[field])
    assert status["partition_2_package_checksum"] == item["package_sha256"]
    assert status["partition_1_package_checksum"] == item["partition_1_package_sha256"]
    assert status["active_version"] == item["package_version"]
    if platform == "amoled_esp32s3":
        assert "stored_bytes=0 blank=1" in coredump_path.read_text()
    else:
        assert "stored_bytes=32 blank=0" in coredump_path.read_text()
        witness = json.loads(checked_file(
            item, "coredump_identity", "coredump_identity_sha256").read_text())
        assert witness["device_uid"] == item["device_uid"]
        assert witness["before_bytes"] == witness["after_bytes"] == 32
        assert witness["byte_identical"] is True
        assert witness["before_sha256"] == witness["after_sha256"] == item["baseline_coredump_sha256"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--allow-pending", action="store_true")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "qualification.json").read_text())
    ids = re.findall(r'H2_PAL_AUDIO_CASE\(\w+, "([^"]+)"\)',
                     (ROOT / "app/include/h2_pal_audio_cases.inc").read_text())
    assert len(ids) == len(set(ids)) == 24
    assert set(manifest["platforms"]) == EXPECTED_PLATFORMS
    assert manifest["interface"]["provider_operations"] == 11
    assert manifest["interface"]["track_operations"] == 5
    assert manifest["interface"]["mandatory_cases"] == 24
    # Keep original artifact observations bound to their original source; a
    # documented review fix has separate current-source and fresh-run evidence.
    current = manifest.get("review_fix_validation", {}).get(
        "current_source_receipts", manifest["source_receipts"])
    assert set(manifest["source_receipts"]).issubset(current)
    for path, expected in current.items():
        assert digest(Path(path)) == expected, ("current source receipt", path)
    if "review_fix_validation" in manifest:
        followup = manifest["review_fix_validation"]
        assert followup["baseline_commit"] == manifest["qualification_snapshot_commit"]
        regression = followup["cleanup_regression"]
        assert regression["old_source_exit"] != 0 and regression["fixed_source_exit"] == 0
        assert regression["retry_limit"] == 3
        path = "projects/e2e/apps/pal-audio/app/src/h2_pal_audio_e2e.c"
        assert regression["original_source_sha256"] == manifest["source_receipts"][path]
        assert regression["fixed_source_sha256"] == current[path]
        for platform in manifest["platforms"].values():
            assert platform["source_binding"] in (
                "source_receipts", "review_fix_validation.current_source_receipts")
    operation_lines = [line for line in
                       (ROOT / "app/include/h2_pal_audio_cases.inc").read_text().splitlines()
                       if line.startswith("H2_PAL_AUDIO_CASE(")]
    assert hashlib.sha256(("\n".join(operation_lines) + "\n").encode()).hexdigest() == \
        manifest["case_registry_operations_sha256"]
    for key in ("e2e_only", "e2e_plus_focused_provider_tests"):
        checked_file(manifest["coverage"][key], "report", "report_sha256")
    budget_log = checked_file(manifest["timeout_regression"], "log", "log_sha256")
    match = re.fullmatch(r"IOS_AUDIO_WAIT budget_ms=(\d+) elapsed_ms=(\d+) "
                         r"unrelated_wakes=(\d+) PASS\n", budget_log.read_text())
    assert match
    budget, elapsed, wakes = (int(value) for value in match.groups())
    assert budget == 80 and 60 <= elapsed < 500 and wakes >= 5
    pending = set()
    for platform, item in manifest["platforms"].items():
        if item["status"] != "PASS":
            pending.add(platform)
            continue
        if platform in ("ios_simulator", "android_emulator"):
            check_mobile(item, ids, platform)
        elif platform in ("amoled_esp32s3", "bk7258"):
            check_board(item, ids, platform)
        else:
            check_text(item, ids, platform)
    if args.allow_pending:
        assert manifest["six_platform_qualified"] == (not pending)
    else:
        assert not pending, pending
        assert manifest["six_platform_qualified"] is True
    print("PAL_AUDIO_QUALIFICATION", "PASS" if not pending else
          "PENDING=" + ",".join(sorted(pending)))


if __name__ == "__main__":
    main()
