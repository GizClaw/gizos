#!/usr/bin/env python3
"""Install the packaged App, require all portable Display cases, retain evidence."""
import argparse
import json
import os
from pathlib import Path
import re

from tools.bazel.mobile_e2e import MobileApp, save_evidence

PACKAGE = "com.haivivi.gizos.e2e.paldisplay"


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_DISPLAY_CASE\("([^"]+)"', registry.read_text())
    assert ids and len(ids) == len(set(ids)), "invalid contract registry"
    expected = dict(platform=platform, contract=1, operations=6, passed=len(ids),
                    failed=0, not_run=0, observations=23, complete=1, qualified=1,
                    rc=0, cleanup=0, teardown=0)
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case["rc"] == 0 for case in report["cases"])


def parse_report(raw):
    lines = raw.splitlines()
    reports = [json.loads(line[18:]) for line in lines if line.startswith("H2_DISPLAY_REPORT ")]
    if not reports:
        raise ValueError("incomplete Display report")
    report = reports[-1]
    report["cases"] = [json.loads(line[16:]) for line in lines if line.startswith("H2_DISPLAY_CASE ")]
    return report


def run_suite(app, args):
    if app.platform == "ios":
        app.verify_ios_symbols(
            executable="GizOSPALDisplayE2E",
            provider_symbols=('_h2_ios_platform_create', '_h2_ios_platform_display_api', '_h2_ios_platform_destroy'),
            portable_symbol="_h2_pal_display_e2e_run",
        )
    else:
        app.verify_android_sdk(
            required_header="prefab/modules/h2_pal_core/include/h2_android_platform.h",
            public_symbols=('h2_android_platform_create', 'h2_android_platform_display_api', 'h2_android_platform_copy_frame', 'h2_android_platform_destroy'),
        )
    return app.launch(parse=parse_report, capture_png=True, android_log="pal-display-result.log")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-display-mobile-result")))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    with MobileApp(args.platform, args.app, args.sdk, PACKAGE,
                   "pal-display-result.log", args.output,
                   timeout=args.timeout) as app:
        result = run_suite(app, args)
        verify(result, args.registry, "ios-simulator" if args.platform == "ios" else "android-emulator")
        save_evidence(args.output, result, app.environment(), args.app, args.sdk)
    count = len(result["cases"])
    print(f"PAL DISPLAY {args.platform}: {count}/{count} PASS, teardown=0")


if __name__ == "__main__":
    main()
