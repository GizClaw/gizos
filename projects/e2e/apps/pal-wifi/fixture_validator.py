"""Verify independent real-peer leases and terminal fixture restoration."""
from pathlib import Path
import re
from receipt_bindings import check_captured_status, check_status, sha


def verify_fixture(report, audit_artifacts=False):
    assert report['status_runs_offset'] == 0
    check_captured_status(report)
    if audit_artifacts:
        check_status(report)
    observations = {}
    for run in report['runs']:
        assert sha(run['log']) == run['sha256']
        text = Path(run['log']).read_text(encoding='utf-8', errors='replace')
        starts = re.findall(r'H2_WIFI_FIXTURE_START window_ms=(\d+) sta_mac=([0-9a-f]{12})', text)
        ends = re.findall(r'H2_WIFI_FIXTURE_END joins=(\d+) cleanup=(-?\d+) settings_unchanged=(\d+) network_restored=(\d+)', text)
        assert len(starts) == len(ends) == 1
        assert int(starts[0][0]) == 600000
        assert ends[0][1:] == ('0', '1', '1')
        assert text.count('H2_WIFI_READY board=amoled-fixture rc=0 confirm=0') == 1
        observations[run['log']] = set()
        clients = re.findall(r'H2_WIFI_FIXTURE_CLIENT target=([^\s]+) joined=(\d+) ip4=(\d+) mac=([0-9a-f]{12})', text)
        assert [int(row[1]) for row in clients] == list(range(1, int(ends[0][0]) + 1)), 'incomplete fixture lease ledger'
        for target, _, ip, mac in clients:
            assert mac == starts[0][1] and int(ip) > 0
            assert target in ('h2wifi-dut-esp', 'h2wifi-dut-bk')
            observations[run['log']].add((target, mac, int(ip)))
    assert any(observations.values()), 'no actual leased fixture client'
    return observations
