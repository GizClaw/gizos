#!/usr/bin/env python3
"""Install the packaged App, require all portable Audio Decoder cases, retain evidence."""
import argparse
import json
import os
from pathlib import Path
import re

from tools.bazel.mobile_e2e import MobileApp, save_evidence

PACKAGE = "com.haivivi.gizos.e2e.palaudiodecoder"


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
    return app.launch()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-audio-decoder-mobile-result")))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    with MobileApp(args.platform, args.app, args.sdk, PACKAGE,
                   "pal-audio-decoder-result.json", args.output,
                   timeout=args.timeout) as app:
        result = run_suite(app, args)
        verify(result, args.registry, "ios-simulator" if args.platform == "ios" else "android-emulator")
        save_evidence(args.output, result, app.environment(), args.app, args.sdk)
    count = len(result["cases"])
    print(f"PAL Audio Decoder {args.platform}: {count}/{count} PASS, blocked=0, teardown=0")


if __name__ == "__main__":
    main()
