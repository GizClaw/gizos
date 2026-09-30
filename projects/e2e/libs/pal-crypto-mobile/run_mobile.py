#!/usr/bin/env python3
"""Install the packaged App, require all portable Crypto cases, retain evidence."""
import re


PACKAGE = "com.haivivi.gizos.e2e.palcrypto"
REPORT = "pal-crypto-result.json"


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
    result = app.launch()
    verify(result, args.registry, args.report_platform)
    return result
