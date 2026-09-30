#!/usr/bin/env python3
"""Install the packaged App, require all portable Display cases, retain evidence."""
import json
import re


PACKAGE = "com.haivivi.gizos.e2e.paldisplay"
REPORT = "pal-display-result.log"


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
    result = app.launch(parse=parse_report, capture_png=True, android_log="pal-display-result.log")
    verify(result, args.registry, args.report_platform)
    return result
