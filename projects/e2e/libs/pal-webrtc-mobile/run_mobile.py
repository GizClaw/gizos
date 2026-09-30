#!/usr/bin/env python3
"""Install the packaged App, require all portable WebRTC cases, retain evidence."""
import json
import os
from pathlib import Path
import re

from fixture import fixture


PACKAGE = "com.haivivi.gizos.e2e.palwebrtc"
REPORT = "pal-webrtc-result.json"
TIMEOUT = 180


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
    verify(result, args.registry, args.report_platform)
    return result


def add_arguments(parser):
    parser.add_argument("server", type=Path)
