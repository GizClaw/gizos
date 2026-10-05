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
# Production receipts stay immutable; the exact opt-in AMOLED DMA delta below
# is separately audited without claiming current physical qualification.
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
SHARED_CATALOG_AUDIT_SOURCES = {
    "projects/e2e/apps/pal-display/check_qualification.py",
    "projects/e2e/apps/pal-display/BUILD.bazel",
    "projects/e2e/apps/pal-display/README.md",
}


# This source admission authenticates only the exact opt-in DMA/diagnostic
# delta. It preserves historical images and never qualifies a current image.
AMOLED_DMA_SOURCE = "boards/amoled/esp32s3/src/h2_esp_board_display.c"
AMOLED_DMA_QUALIFIED_SHA256 = "834fe172281d46519c0039aa79b190af85f1c6892f90709fc760f6216db7c001"
AMOLED_DMA_EXECUTION_COMMIT = "1eed91f5ddacca5bcc901b6096284b18e2ffd727"
AMOLED_DMA_LOG_SHA256 = "03c0443704c816addc2d167185ecc3bcadf47ec52f3432e6148906a113931012"
AMOLED_DMA_BINARY_SHA256 = "daf0aa4d3de51691b84672bfc6605584edf7ad70d23b13da75b8a92703d605a7"
AMOLED_DMA_INCLUDE = b'#include "h2_esp_board_display_config_internal.h"\n'
AMOLED_DMA_OLD_LOOP = b'        for (int rows = LCD_DRAW_ROWS; rows >= LCD_DRAW_ROWS_MIN; rows /= 2) {\n'
AMOLED_DMA_NEW_LOOP = (b'        int ceiling = (int)h2_esp_board_display_dma_rows(\n'
                       b'            &s_display_config, LCD_DRAW_ROWS);\n'
                       b'        for (int rows = ceiling; rows >= LCD_DRAW_ROWS_MIN; rows /= 2) {\n')
AMOLED_DMA_INPUT_SHA256 = {'boards/amoled/esp32s3/BUILD.bazel': 'd0d66bf6e362fac981b6a8e5b80b4ac5caff4a56f1d6484152e64edf59fd6719',
 'boards/amoled/esp32s3/CMakeLists.txt': '36993f7696860a40c545fd71e00d2fdc8b19eecf75b9dbe92e78dcab2194ea2f',
 'boards/amoled/esp32s3/idf_component.yml': '842c1305b4e86caa90f2ddc347a10e529954eccc17f5d1744d27e56655dd484d',
 'boards/amoled/esp32s3/include/h2_esp_board.h': '82b9cf36925d56e6cc57b7f3cd8a88d67603865d8afed7ec0b668e6837a2643d',
 'boards/amoled/esp32s3/sdkconfig.defaults': '9f831f6776b47d84e266a2cbb72947b4abe87a4ccd345742f07bae9b1bfbd6b9',
 'boards/amoled/esp32s3/src/h2_esp_board_display_config.c': '1eec9ab5a885fa44a18c69a9f21ce258cb63a6dbd38d35d2a3510b15e3d747c4',
 'boards/amoled/esp32s3/src/h2_esp_board_display_config_internal.h': 'b54ed1def340a3bf1384ed81c782cd67c1d27c978cf18a4243a70f9ff2b9cb9d',
 'boards/amoled/esp32s3/src/h2_esp_board_touch_ft3168.c': 'd91b2adc40017b76549a08f4951e15015a2ee14d8c8c1b709d93c98fde947def',
 'boards/amoled/esp32s3/tests/test_h2_esp_board_display_config.c': 'ce25d51d8d7845653e9f60492e51c631b511771a4af04ed97e9199fdc144ffe4'}
AMOLED_TOUCH_BASELINE_SHA256 = "bd4643c3e5152e4ffeafbc991ad23d635c436bae7eaf33c092f6dbf29230eae2"
AMOLED_TOUCH_DIAGNOSTIC_EDITS = [(b'    bool contact_active;\n', b'    bool contact_active;\n    bool read_error_reported;\n'),
 (b'    s_touch.contact_active = false;\n    ESP_LOGI(TAG, "FT3168 ready',
  b'    s_touch.contact_active = false;\n    s_touch.read_error_reported = false;\n    ESP_LOGI(TA'
  b'G, "FT3168 ready'),
 (b'    if (err != ESP_OK) {\n        return map_esp_err(err);\n    }\n    const uint8_t contact_co'
  b'unt',
  b'    if (err != ESP_OK) {\n        if (!s_touch.read_error_reported) {\n            ESP_LOGW(TA'
  b'G, "FT3168 touch read failed: %s", esp_err_to_name(err));\n            s_touch.read_error_rep'
  b'orted = true;\n        }\n        return map_esp_err(err);\n    }\n    s_touch.read_error_re'
  b'ported = false;\n    const uint8_t contact_count')]


def amoled_dma_maintenance_sources(previous):
    record = json.JSONDecoder().decode(
        (ROOT / "amoled_dma_maintenance.json").read_text(encoding="utf-8"))
    assert record["schema"] == 1
    assert record["new_physical_run_claimed"] is False
    assert record["current_physical_qualification"] is False
    assert record["historical_qualification_sha256"] == hashlib.sha256(
        (ROOT / "qualification.json").read_bytes()).hexdigest()
    assert record["qualified_driver_sha256"] == previous[AMOLED_DMA_SOURCE] == AMOLED_DMA_QUALIFIED_SHA256
    assert record["closed_input_sha256"] == AMOLED_DMA_INPUT_SHA256
    for path, expected in AMOLED_DMA_INPUT_SHA256.items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected, path
    content = Path(AMOLED_DMA_SOURCE).read_bytes()
    assert content.count(AMOLED_DMA_INCLUDE) == content.count(AMOLED_DMA_NEW_LOOP) == 1
    restored = content.replace(AMOLED_DMA_NEW_LOOP, AMOLED_DMA_OLD_LOOP, 1)
    restored = restored.replace(AMOLED_DMA_INCLUDE, b"", 1)
    assert hashlib.sha256(restored).hexdigest() == previous[AMOLED_DMA_SOURCE], (
        "unlisted Display code/default/flags changed")
    current_sha = hashlib.sha256(content).hexdigest()
    assert record["current_driver_sha256"] == current_sha
    touch = Path("boards/amoled/esp32s3/src/h2_esp_board_touch_ft3168.c").read_bytes()
    for before, after in reversed(AMOLED_TOUCH_DIAGNOSTIC_EDITS):
        assert touch.count(after) == 1
        touch = touch.replace(after, before, 1)
    assert hashlib.sha256(touch).hexdigest() == AMOLED_TOUCH_BASELINE_SHA256, (
        "Touch maintenance is diagnostics only")
    execution = record["host_validation"]
    assert execution["default_dma_rows"] == 64 and execution["status"] == "PASS"
    assert execution["current_driver_physical_test"] == "NOT_RUN"
    assert execution["executed_source_commit"] == AMOLED_DMA_EXECUTION_COMMIT
    assert execution["raw_log_sha256"] == AMOLED_DMA_LOG_SHA256
    assert execution["test_binary_sha256"] == AMOLED_DMA_BINARY_SHA256
    assert execution["source_sha256"] == AMOLED_DMA_INPUT_SHA256
    return {**previous, AMOLED_DMA_SOURCE: current_sha}


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
    assert audit["previous_source_sha256"] == {
        path: previous[path] for path in SHARED_CATALOG_AUDIT_SOURCES}
    replacements = audit["current_source_sha256"]
    assert set(replacements) == SHARED_CATALOG_AUDIT_SOURCES
    maintenance = shared_ipv6_maintenance()
    assert maintenance["previous_audit_sha256"] == replacements
    updates = maintenance["current_audit_sha256"]
    assert set(updates) == SHARED_CATALOG_AUDIT_SOURCES
    sources = {**previous, **replacements, **updates}
    network = maintenance.get("network_config_changes", {})
    assert set(network) <= {"boards/bk7258_v3_202405/bk7258/ap.defaults"}
    for path, change in network.items():
        assert change["previous_sha256"] == previous[path]
        content = Path(path).read_bytes()
        assert hashlib.sha256(content).hexdigest() == change["current_sha256"]
        before, after = change["before_utf8"].encode(), change["after_utf8"].encode()
        assert after and content.count(after) == 1
        assert before == b"# CONFIG_IPV6 is not set\n"
        assert after == b"CONFIG_IPV6=y\n"
        restored = content.replace(after, before, 1)
        assert hashlib.sha256(restored).hexdigest() == previous[path]
        sources[path] = change["current_sha256"]
    sources = amoled_dma_maintenance_sources(sources)
    # The historical whole-file hash still authenticates the immutable baseline.
    # Other Apps may update their catalog entries without a new Display run.
    del sources[SHARED_CATALOG]
    del sources[SHARED_PAL_GUIDE]
    return sources


SHARED_PAL_IPV6_BASELINE_SHA256 = "36480ff0007d77faeb060ddd7ddc753dbf1bc4110244792a7312487d8b0381ca"
SHARED_PAL_IPV6_OWNERS = {"net_ipv6", "wifi_ipv6_readiness",
                         "wifi_ipv6_persistence"}


def shared_ipv6_maintenance():
    record = json.JSONDecoder().decode(
        (ROOT / "shared_ipv6_maintenance.json").read_text(encoding="utf-8"))
    assert record["schema"] == 1 and record["new_physical_run_claimed"] is False
    assert record["historical_shared_catalog_provenance_sha256"] == hashlib.sha256(
        (ROOT / "shared_catalog_provenance.json").read_bytes()).hexdigest()
    assert record["historical_baseline_commit"] == "e9ee7e6b4d2a15b70bb3a7a9147bacf3937365c3"
    assert record["historical_baseline_sha256"] == SHARED_PAL_IPV6_BASELINE_SHA256
    return record


def shared_pal_before_ipv6(content):
    """Reverse only explicitly recorded owned deltas; all other bytes stay exact."""
    record = shared_ipv6_maintenance()
    owners = [change["owner"] for change in record["guide_changes"]]
    assert len(owners) == len(set(owners)) and set(owners) <= SHARED_PAL_IPV6_OWNERS
    assert "net_ipv6" in owners
    for change in reversed(record["guide_changes"]):
        before, after = change["before_utf8"].encode(), change["after_utf8"].encode()
        assert after and before != after
        owner = change["owner"]
        if owner == "net_ipv6":
            assert before == b"" and after.decode().startswith("\n## IPv6 地址与 DNS\n")
        elif owner == "wifi_ipv6_readiness":
            assert before == b"" and after.decode().startswith("## Wi-Fi IPv6 就绪\n")
        elif owner == "wifi_ipv6_persistence":
            assert before.decode().startswith("Wi-Fi STA 的 `connect` 与 `connect_and_save`")
            assert after.decode().startswith("Wi-Fi STA 的 `connect` 与 `connect_and_save`")
            assert "\n\n" not in before.decode().strip() and "\n\n" not in after.decode().strip()
        else:
            raise AssertionError("unowned shared PAL change")
        assert content.count(after) == 1, "recorded guide delta missing or duplicated: " + owner
        content = content.replace(after, before, 1)
    return content


def shared_pal_guide(previous, extension):
    """After owned IPv6 reversal, require the exact historical BK Pref insertion."""
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
    current = Path(SHARED_PAL_GUIDE).read_bytes()
    current = shared_pal_before_ipv6(current)
    if hashlib.sha256(current).hexdigest() == previous[SHARED_PAL_GUIDE]:
        return
    offset = extension["insertion_offset"]
    assert type(offset) is int and 0 <= offset < len(current)
    assert current[offset:offset + len(addition)] == addition
    restored = current[:offset] + current[offset + len(addition):]
    assert hashlib.sha256(restored).hexdigest() == previous[SHARED_PAL_GUIDE], (
        "shared PAL guide changed outside the exact BK Pref addition")


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
    print("PAL Display: historical six-platform receipts and exact source maintenance verified; current AMOLED DMA source/artifact is not physically requalified")


if __name__ == "__main__":
    main()
