#!/usr/bin/env python3
"""Install the packaged Audio PAL App and verify its mandatory case ledger."""
import re


PACKAGE = "com.haivivi.gizos.e2e.palaudio"
REPORT = "pal-audio-result.json"


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_AUDIO_CASE\(\w+, "([^"]+)"\)', registry.read_text())
    assert len(ids) == len(set(ids)) == 24, "unexpected Audio registry"
    assert report["platform"] == platform
    assert report["image_version"] == "2.0.0"
    assert [item["id"] for item in report["cases"]] == ids
    assert all(item["status"] == "PASS" and item["detail"] == 0
               for item in report["cases"])
    for key, value in {"passed": 24, "failed": 0, "blocked": 0,
                       "run_rc": 0, "teardown_rc": 0}.items():
        assert report[key] == value, (key, report)
    assert report["mic_frames"] >= 2 and report["speaker_frames"] >= 2
    assert report["stability_elapsed_ms"] >= 30000
    assert report["output_peak"] > 0
    assert report["allocator_probe_passed"] == report["allocator_failure_rejected"] == 1
    assert report["allocator_allocations"] > 0
    assert report["allocator_allocations"] == report["allocator_frees"]


def run_suite(app, args):
    if app.platform == "ios":
        app.simctl("privacy", app.device, "grant", "microphone", PACKAGE)
    else:
        app.adb_command("shell", "pm", "grant", PACKAGE, "android.permission.RECORD_AUDIO")
    result = app.launch()
    verify(result, args.registry, args.report_platform)
    return result
