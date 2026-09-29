#!/usr/bin/env python3
"""Install the packaged App, require all portable HTTP cases, retain evidence."""
import argparse
import json
import os
from pathlib import Path
import re
import tempfile

from fixture import Fixture

from tools.bazel.mobile_e2e import MobileApp, save_evidence

PACKAGE = "com.haivivi.gizos.e2e.palhttp"


def verify(report, registry, platform):
    ids = re.findall(r'H2_PAL_HTTP_CASE\(\w+, "([^"]+)"\)', registry.read_text())
    assert len(ids) == 45 and len(set(ids)) == 45, "unexpected contract registry"
    expected = dict(platform=platform, version="2.0.0", contract=1, operations=2, passed=len(ids),
                    failed=0, blocked=0, retained_allocations=0, rc=0, teardown=0)
    for key, value in expected.items():
        assert report.get(key) == value, f"{key}: expected {value}, got {report.get(key)}"
    assert [case["id"] for case in report["cases"]] == ids, "case ledger differs from registry"
    assert all(case["status"] == "PASS" and case["detail"] == 0 for case in report["cases"])


def run_suite(app, args):
    advertised = "127.0.0.1" if args.platform == "ios" else "10.0.2.2"
    with tempfile.TemporaryDirectory(prefix="pal-http-fixture-") as directory, Fixture(directory, advertised=advertised) as fixture:
        settings = dict(http=fixture.http, https=fixture.https, untrusted=fixture.untrusted,
                        ca=fixture.ca.read_text())
        with app.fixture("fixture.json", json.dumps(settings)):
            result = app.launch()
        app.environment()["fixture_attempts"] = fixture.verify_arrivals()
        app.environment()["tls_rejection"] = fixture.verify_tls_rejection()
        app.environment()["tls_verification"] = "required; explicit isolated test CA; separate untrusted certificate rejected"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["ios", "android"])
    parser.add_argument("app", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", "/tmp/pal-http-mobile-result")))
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    with MobileApp(args.platform, args.app, args.sdk, PACKAGE,
                   "pal-http-result.json", args.output,
                   timeout=args.timeout) as app:
        result = run_suite(app, args)
        verify(result, args.registry, "ios-simulator" if args.platform == "ios" else "android-emulator")
        save_evidence(args.output, result, app.environment(), args.app, args.sdk)
    print(f"PAL HTTP {args.platform}: 45/45 PASS, blocked=0, teardown=0")


if __name__ == "__main__":
    main()
