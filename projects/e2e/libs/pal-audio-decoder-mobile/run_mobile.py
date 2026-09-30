#!/usr/bin/env python3
"""Install the packaged App, require all portable Audio Decoder cases, retain evidence."""
import re


PACKAGE = "com.haivivi.gizos.e2e.palaudiodecoder"
REPORT = "pal-audio-decoder-result.json"


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_ADEC_CASE\("([^"]+)"', registry.read_text())
    assert ids and len(ids) == len(set(ids)), "invalid contract registry"
    expected = dict(platform=platform, version="2.0.0", contract=1, operations=8, passed=len(ids),
                    failed=0, blocked=0, retained=0, qualified=1,
                    rc=0, teardown=0)
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case["detail"] == 0 for case in report["cases"])


def run_suite(app, args):
    if app.platform == "ios":
        app.verify_ios_symbols(
            executable="GizOSPALAudioDecoderE2E",
            provider_symbols=('_h2_ios_platform_audio_decoder_api',),
            portable_symbol="_h2_pal_audio_decoder_e2e_run",
        )
    else:
        app.verify_android_sdk(
            required_header="prefab/modules/h2_pal_core/include/h2_android_platform.h",
            public_symbols=('h2_android_platform_audio_decoder_api',),
        )
    result = app.launch()
    verify(result, args.registry, args.report_platform)
    return result
