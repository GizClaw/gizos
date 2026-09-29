#!/usr/bin/env python3
"""Install the packaged Audio PAL App and verify its mandatory case ledger."""
import argparse
import json
import os
from pathlib import Path
import re

from tools.bazel.mobile_e2e import MobileApp, save_evidence

PACKAGE = "com.haivivi.gizos.e2e.palaudio"


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
    return app.launch()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR",
                                                    "/tmp/pal-audio-mobile-result")))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    with MobileApp(args.platform, args.app, args.sdk, PACKAGE,
                   "pal-audio-result.json", args.output,
                   timeout=args.timeout) as app:
        result = run_suite(app, args)
        verify(result, args.registry,
               "ios-simulator" if args.platform == "ios" else "android-emulator")
        save_evidence(args.output, result, app.environment(), args.app, args.sdk)
    print(f"PAL Audio {args.platform}: 24/24 PASS blocked=0")


if __name__ == "__main__":
    main()
