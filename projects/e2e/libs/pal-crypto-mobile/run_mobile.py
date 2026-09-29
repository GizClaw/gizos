#!/usr/bin/env python3
"""Install the packaged App, require all portable Crypto cases, retain evidence."""
import argparse
import json
import os
from pathlib import Path
import re

from tools.bazel.mobile_e2e import MobileApp, save_evidence

PACKAGE = "com.haivivi.gizos.e2e.palcrypto"


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_CRYPTO_CASE\("([^"]+)"', registry.read_text())
    assert len(ids) == 22 and len(set(ids)) == 22, "unexpected contract registry"
    expected = dict(platform=platform, version="2.0.0", contract=1, operations=15, passed=len(ids),
                    failed=0, blocked=0, not_run=0, complete=1, qualified=1,
                    rc=0, teardown=0)
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case["rc"] == 0 for case in report["cases"])


def run_suite(app, args):
    return app.launch()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-crypto-mobile-result")))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    with MobileApp(args.platform, args.app, args.sdk, PACKAGE,
                   "pal-crypto-result.json", args.output,
                   timeout=args.timeout) as app:
        result = run_suite(app, args)
        verify(result, args.registry, "ios-simulator" if args.platform == "ios" else "android-emulator")
        save_evidence(args.output, result, app.environment(), args.app, args.sdk)
    print(f"PAL Crypto {args.platform}: 22/22 PASS, blocked=0, teardown=0")


if __name__ == "__main__":
    main()
