#!/usr/bin/env python3
"""Install the packaged App, require all portable WebRTC cases, retain evidence."""
import argparse
import json
import os
from pathlib import Path
import re

from fixture import fixture

from tools.bazel.mobile_e2e import MobileApp, save_evidence

PACKAGE = "com.haivivi.gizos.e2e.palwebrtc"


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_WEBRTC_CASE\(\w+, "([^"]+)"\)', registry.read_text())
    assert bool(ids) and len(set(ids)) == len(ids), "unexpected contract registry"
    expected = dict(platform=platform, version="2.0.0", contract=1, operations=13, passed=len(ids),
                    failed=0, blocked=0, retained_allocations=0, rc=0, teardown=0)
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case["detail"] == 0 for case in report["cases"])
    authentication = next(case for case in report["cases"] if case["id"] == "fingerprint-rejected")
    assert authentication.get("observed_error") == -17 and authentication.get("authentication_evidence") == 1, "missing precise native fingerprint authentication failure"


def run_suite(app, args):
    address = "127.0.0.1" if args.platform == "ios" else os.environ.get("H2_WEBRTC_FIXTURE_IP")
    if not address:
        raise ValueError("Android requires explicit host LAN H2_WEBRTC_FIXTURE_IP for UDP ICE")
    with fixture(args.server, address) as settings:
        with app.fixture("fixture.json", json.dumps(settings)):
            result = app.launch()
        app.environment()["fixture"] = "isolated Pion; real UDP ICE/DTLS/SRTP/SCTP"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("server", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-webrtc-mobile-result")))
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    with MobileApp(args.platform, args.app, args.sdk, PACKAGE,
                   "pal-webrtc-result.json", args.output,
                   timeout=args.timeout) as app:
        result = run_suite(app, args)
        verify(result, args.registry, "ios-simulator" if args.platform == "ios" else "android-emulator")
        save_evidence(args.output, result, app.environment(), args.app, args.sdk)
    print(f"PAL WebRTC {args.platform}: all cases PASS, blocked=0, teardown=0")


if __name__ == "__main__":
    main()
