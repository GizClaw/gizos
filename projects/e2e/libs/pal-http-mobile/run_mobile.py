#!/usr/bin/env python3
"""Install the packaged App, require all portable HTTP cases, retain evidence."""
import json
import re
import tempfile

from fixture import Fixture


PACKAGE = "com.haivivi.gizos.e2e.palhttp"
REPORT = "pal-http-result.json"
TIMEOUT = 180


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
    verify(result, args.registry, args.report_platform)
    return result
