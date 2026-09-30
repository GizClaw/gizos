#!/usr/bin/env python3
"""Install the packaged App, require all portable Json cases, retain evidence."""
import re


PACKAGE = "com.haivivi.gizos.e2e.paljson"
REPORT = "pal-json-result.json"


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_JSON_CASE\("([^"]+)"', registry.read_text())
    assert ids and len(ids) == len(set(ids)), "invalid contract registry"
    expected = dict(platform=platform, version="2.0.0", contract=1, operations=24, passed=len(ids),
                    failed=0, blocked=0, not_run=0, complete=1, qualified=1,
                    rc=0, teardown=0)
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case["rc"] == 0 for case in report["cases"])


def run_suite(app, args):
    if app.platform == "ios":
        app.verify_ios_symbols(
            executable="GizOSPALJsonE2E",
            provider_symbols=('_h2_yyjson_json_create', '_h2_yyjson_json_api', '_h2_yyjson_json_destroy'),
            portable_symbol="_h2_pal_json_e2e_run",
            header_member="H2PALCore.xcframework/ios-arm64-simulator/H2PALCore.framework/Headers/h2_yyjson_json.h",
        )
    else:
        app.verify_android_sdk(
            required_header="prefab/modules/h2_pal_core/include/h2_yyjson_json.h",
            public_symbols=('h2_android_json_provider_create', 'h2_android_json_provider_destroy', 'h2_yyjson_json_create', 'h2_yyjson_json_api', 'h2_yyjson_json_destroy'),
        )
    result = app.launch()
    verify(result, args.registry, args.report_platform)
    return result
