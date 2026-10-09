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
    "projects/e2e/libs/pal-display-mobile/BUILD.bazel",
    "projects/e2e/targets/android_binary/pal-display/BUILD.bazel",
    "projects/e2e/targets/ios_application/pal-display/BUILD.bazel",
    "tools/bazel/mobile_e2e.py",
    "tools/bazel/mobile_e2e.bzl",
}
REMOVED_RUNNERS = {
    "projects/e2e/targets/android_binary/pal-display/run_simulator.sh",
    "projects/e2e/targets/ios_application/pal-display/run_simulator.sh",
}
AUDIT_SOURCES = {
    "Makefile",
    "guides/apps/e2e.md",
    "guides/zh/developing/platform_abstract_layer.md",
}
GIZCLAW_HARNESS_SOURCES = AUDIT_SOURCES | {
    "tools/bazel/mobile_e2e.py",
    "projects/e2e/apps/pal-display/check_qualification.py",
    "projects/e2e/apps/pal-display/BUILD.bazel",
}
SHARED_CATALOG = "guides/apps/e2e.md"
SHARED_PAL_GUIDE = "guides/zh/developing/platform_abstract_layer.md"
SHARED_CATALOG_BASELINE_COMMIT = "06f9c0cfa633646984d72f210ba18f889bbec528"
SHARED_PAL_ADDITION_COMMIT = "93c9578e54f1cd45d8d9bd118372f807e16076f4"
SHARED_PAL_ADDITION_SHA256 = "e34e59d847676c1d0bca583f8c824a124b1a8cb0dd684b097dcdc25aff429932"
SHARED_PAL_ADDITION_OFFSET = 72468
SHARED_PAL_MODEM_BASELINE_SHA256 = "78e3d827b63a6c85f92c1b913058530e1bfe5fdbbba45f1709ab7e1836ec7d3d"
SHARED_CATALOG_AUDIT_SOURCES = {
    "projects/e2e/apps/pal-display/check_qualification.py",
    "projects/e2e/apps/pal-display/BUILD.bazel",
    "projects/e2e/apps/pal-display/README.md",
}


def display_catalog_content(text):
    """Extract the complete owned section and unique launcher-matrix row."""
    headings = list(re.finditer(r"^## .*\n", text, re.MULTILINE))
    def section(title):
        matches = [index for index, heading in enumerate(headings)
                   if heading.group() == f"## {title}\n"]
        assert len(matches) == 1, f"missing or duplicate {title} catalog section"
        index = matches[0]
        end = headings[index + 1].start() if index + 1 < len(headings) else len(text)
        return text[headings[index].end():end]

    display = "## PAL Display\n" + section("PAL Display")
    apps = section("Apps")
    # A copied row in prose, a code sample or another section is not a table entry.
    visible = []
    fence = None
    for line in apps.splitlines(keepends=True):
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})", line)
        if fence:
            if marker and marker.group(1)[0] == fence[0] and len(marker.group(1)) >= len(fence) and not line[marker.end():].strip():
                fence = None
            visible.append("\n")
        elif marker:
            fence = marker.group(1)
            visible.append("\n")
        else:
            visible.append(line)
    tables = re.findall(
        r"^\| App \| Portable target \| Current launcher matrix \|\n"
        r"\| --- \| --- \| --- \|\n(?:\|[^\n]*\n)+",
        "".join(visible), re.MULTILINE)
    assert len(tables) == 1, "missing or duplicate Apps launcher matrix"
    rows = re.findall(r"^\| PAL Display \|[^\n]*\n", text, re.MULTILINE)
    assert len(rows) == 1, "missing or duplicate Display catalog row"
    assert rows == re.findall(r"^\| PAL Display \|[^\n]*\n", tables[0], re.MULTILINE), (
        "Display row is outside the Apps launcher matrix")
    return {"section": display, "launcher_row": rows[0]}


def retired_pal_makefile_source(content, expected):
    """Admit only the removed legacy MQTT alias against the original hash."""
    current = hashlib.sha256(content).hexdigest()
    if current == expected:
        return current
    prefix = b".PHONY: help cfg-doctor bazel-build bazel-test bazel-test-downstream-consumer"
    retired = b" bazel-test-mqtt_public_broker_smoke"
    anchor = b"\nbazel-coverage-report:\n"
    target = (b"\nbazel-test-mqtt_public_broker_smoke:\n"
              b"\t@scripts/bazel/bazel-test-mqtt_public_broker_smoke.sh\n")
    assert content.count(prefix + b" bazel-coverage-report") == 1
    assert content.count(anchor) == 1 and retired not in content
    restored = content.replace(prefix, prefix + retired, 1)
    restored = restored.replace(anchor, target + anchor, 1)
    assert hashlib.sha256(restored).hexdigest() == expected, (
        "Makefile changed outside the retired PAL MQTT alias")
    return current


def shared_catalog_sources(previous):
    """Preserve historical receipts while checking only owned catalog bytes."""
    audit = json.JSONDecoder().decode(
        (ROOT / "shared_catalog_provenance.json").read_text(encoding="utf-8"))
    assert audit["schema"] == 1 and audit["new_physical_run_claimed"] is False
    assert audit["historical_harness_receipt_sha256"] == hashlib.sha256(
        (ROOT / "gizclaw_harness_provenance.json").read_bytes()).hexdigest()
    baseline = audit["catalog_baseline"]
    assert baseline["source_commit"] == SHARED_CATALOG_BASELINE_COMMIT
    assert baseline["source_path"] == SHARED_CATALOG
    assert baseline["source_sha256"] == previous[SHARED_CATALOG]
    content = (ROOT / "shared_catalog_baseline.txt").read_bytes()
    assert hashlib.sha256(content).hexdigest() == previous[SHARED_CATALOG]
    assert display_catalog_content(content.decode("utf-8")) == display_catalog_content(
        Path(SHARED_CATALOG).read_text(encoding="utf-8")), "Display catalog content changed"
    shared_pal_guide(previous, audit["pal_guide_extension"])
    assert audit["pal_modem_scope"] == {
        "source_commit": SHARED_CATALOG_BASELINE_COMMIT,
        "source_path": SHARED_PAL_GUIDE,
        "baseline_sha256": SHARED_PAL_MODEM_BASELINE_SHA256,
        "scope": "Only Modem volume and emergency/OTA sections; no Display or general PAL policy changes",
    }
    assert audit["previous_source_sha256"] == {
        path: previous[path] for path in SHARED_CATALOG_AUDIT_SOURCES}
    replacements = audit["current_source_sha256"]
    assert set(replacements) == SHARED_CATALOG_AUDIT_SOURCES
    sources = {**previous, **replacements}
    sources["Makefile"] = retired_pal_makefile_source(
        Path("Makefile").read_bytes(), previous["Makefile"])
    # The historical whole-file hash still authenticates the immutable baseline.
    # Other Apps may update their catalog entries without a new Display run.
    del sources[SHARED_CATALOG]
    del sources[SHARED_PAL_GUIDE]
    return sources


def modem_guide_section(content):
    """A closed Modem-only region, bounded by the historical following section."""
    start_marker = "### Modem 通话扬声器音量\n".encode("utf-8")
    end_marker = b"### Touch\n"
    assert content.count(start_marker) == 1 and content.count(end_marker) == 1, (
        "missing or duplicate Modem/Touch section boundary")
    start, end = content.index(start_marker), content.index(end_marker)
    assert start < end, "Modem section moved past Touch"
    region = content[start:end]
    # CommonMark permits tabs after hashes, up to three leading spaces and
    # empty ATX headings. Detect those forms even though only our exact two
    # Modem headings are admitted. Setext headings are outside this profile.
    headings = re.findall(rb"^ {0,3}#{1,6}(?:[ \t]+[^\r\n]*|)(?:\r?\n|\Z)", region, re.MULTILINE)
    assert not re.search(rb"^ {0,3}(?:=+|-+)[ \t]*\r?$", region, re.MULTILINE), (
        "Setext heading or underline inside Modem ownership region")
    allowed = [start_marker, "### Modem 紧急号码和固件升级\n".encode("utf-8")]
    assert headings in (allowed[:1], allowed), "non-Modem section inside Modem ownership region"
    return start, end


def historical_modem_guide(content):
    """Project independent Modem docs onto authenticated historical Display input.

    All bytes outside this closed region remain subject to the original
    whole-file digest; this does not requalify any production source or board.
    """
    record = json.JSONDecoder().decode(
        (ROOT / "shared_pal_modem_baseline.json").read_text(encoding="utf-8"))
    assert record["source_commit"] == SHARED_CATALOG_BASELINE_COMMIT
    assert record["source_path"] == SHARED_PAL_GUIDE
    baseline = record["section_utf8"].encode("utf-8")
    assert hashlib.sha256(baseline).hexdigest() == SHARED_PAL_MODEM_BASELINE_SHA256, (
        "historical Modem baseline changed")
    start, end = modem_guide_section(content)
    return content[:start] + baseline + content[end:]


def shared_pal_guide(previous, extension):
    """Preserve the whole historical guide outside independent Modem docs."""
    assert extension["source_commit"] == SHARED_CATALOG_BASELINE_COMMIT
    assert extension["source_path"] == SHARED_PAL_GUIDE
    assert extension["addition_source_commit"] == SHARED_PAL_ADDITION_COMMIT
    assert extension["addition_sha256"] == SHARED_PAL_ADDITION_SHA256
    assert type(extension["insertion_offset"]) is int
    assert extension["insertion_offset"] == SHARED_PAL_ADDITION_OFFSET
    assert extension["source_sha256"] == previous[SHARED_PAL_GUIDE]
    record = json.JSONDecoder().decode(
        (ROOT / "shared_pal_pref_addition.json").read_text(encoding="utf-8"))
    assert record["source_commit"] == SHARED_PAL_ADDITION_COMMIT
    assert record["source_path"] == SHARED_PAL_GUIDE
    addition = record["addition_utf8"].encode("utf-8")
    assert hashlib.sha256(addition).hexdigest() == SHARED_PAL_ADDITION_SHA256
    paragraphs = addition.decode("utf-8").strip().split("\n\n")
    assert len(paragraphs) == 2
    assert paragraphs[0].startswith("BK7258 大值仍使用原 128 KiB physical FlashDB 分区：")
    assert paragraphs[1].startswith("Tail v1 manifest 为 96 字节：")
    current = historical_modem_guide(Path(SHARED_PAL_GUIDE).read_bytes())
    if hashlib.sha256(current).hexdigest() == previous[SHARED_PAL_GUIDE]:
        return
    offset = extension["insertion_offset"]
    assert type(offset) is int and 0 <= offset < len(current)
    assert current[offset:offset + len(addition)] == addition
    restored = current[:offset] + current[offset + len(addition):]
    assert hashlib.sha256(restored).hexdigest() == previous[SHARED_PAL_GUIDE], (
        "shared PAL guide changed outside the exact BK Pref addition and Modem-owned sections")


def runner_refactor(historical):
    """Check new mobile observations without relabeling old board evidence."""
    followup = json.loads((ROOT / "mobile_runner_refactor.json").read_text(encoding="utf-8"))
    assert followup["schema"] == 1
    assert followup["new_physical_run_claimed"] is False
    assert re.fullmatch(r"[0-9a-f]{40}", followup["mobile_execution_commit"])
    assert hashlib.sha256((ROOT / "qualification.json").read_bytes()).hexdigest() == followup["historical_qualification_sha256"]
    current = followup["current_source_sha256"]
    assert set(current) == RUNNER_SOURCES
    audit = followup["audit_source_sha256"]
    assert set(audit) == AUDIT_SOURCES
    assert set(followup["removed_source_sha256"]) == REMOVED_RUNNERS
    # Later harness changes keep the executed mobile and physical receipts
    # unchanged. This separate record may supersede only host harness/audit
    # source hashes, and binds both the old and new byte identities.
    maintenance = json.JSONDecoder().decode(
        (ROOT / "gizclaw_harness_provenance.json").read_text(encoding="utf-8"))
    assert maintenance["schema"] == 1
    assert maintenance["new_physical_run_claimed"] is False
    assert maintenance["historical_runner_receipt_sha256"] == hashlib.sha256(
        (ROOT / "mobile_runner_refactor.json").read_bytes()).hexdigest()
    previous = {**historical, **current, **audit}
    replacements = maintenance["current_source_sha256"]
    assert set(replacements) == set(maintenance["previous_source_sha256"])
    assert set(replacements) <= GIZCLAW_HARNESS_SOURCES
    assert all(previous[path] == sha for path, sha in
               maintenance["previous_source_sha256"].items())
    assert maintenance["validation"]["shared_mobile_contract"] == "PASS"
    for path, expected in followup["removed_source_sha256"].items():
        assert historical[path] == expected and not Path(path).exists(), path
    for path, expected in shared_catalog_sources({**previous, **replacements}).items():
        if path not in REMOVED_RUNNERS:
            assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected, path
    # These exact Python sources and consumer declarations executed the stored runs.
    executed = followup["executed_runner_sha256"]
    assert set(executed) == {
        "tools/bazel/mobile_e2e.py",
        "projects/e2e/libs/pal-display-mobile/run_mobile.py",
        "projects/e2e/libs/pal-display-mobile/BUILD.bazel",
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
        declaration = Path("projects/e2e/libs/pal-display-mobile/mobile_e2e.json")
        assert environment["suite_sha256"] == hashlib.sha256(declaration.read_bytes()).hexdigest()


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
