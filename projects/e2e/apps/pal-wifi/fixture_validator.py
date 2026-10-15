"""Verify independent real-peer leases and terminal fixture restoration."""
from pathlib import Path
import json,re
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
        observations[run['log']] = []
        clients = re.findall(r'H2_WIFI_FIXTURE_CLIENT target=([^\s]+) joined=(\d+) ip4=(\d+) mac=([0-9a-f]{12})', text)
        assert [int(row[1]) for row in clients] == list(range(1, int(ends[0][0]) + 1)), 'incomplete fixture lease ledger'
        for target, _, ip, mac in clients:
            assert mac == starts[0][1] and int(ip) > 0
            assert target in ('h2wifi-dut-esp', 'h2wifi-dut-bk')
            observations[run['log']].append((target, mac, int(ip)))
    assert any(observations.values()), 'no actual leased fixture client'
    return observations


def verify_peer_witnesses(dut_log, fixture_log, target, mac, repeats=2):
    """Match one or two real peers per AP mode by independent clock offset.

    The first DUT lease and the first fixture CLIENT establish the offset.
    WPA2/open/hidden ADDRESS markers and the remaining CLIENT times must then
    agree with that one offset and their ordered case windows. Two peers per
    mode is mandatory for final images; one supports a source-bound R31 seed.
    """
    dut = Path(dut_log).read_text(encoding='utf-8', errors='replace')
    fixture = Path(fixture_log).read_text(encoding='utf-8', errors='replace')
    addresses = [(int(ms), int(ip), int(ap), int(mask)) for ms, ip, ap, mask in
                 re.findall(r'I\s*\((\d+)\).*H2_WIFI_ADDRESS lease=(\d+) ap=(\d+) mask=(\d+)', dut)]
    assert len(addresses) == 3, 'missing WPA2/open/hidden lease observations'
    for _, ip, ap, mask in addresses:
        assert mask and (ip & mask) == (ap & mask), 'DUT lease outside AP subnet'
    cases = {}
    for ms, payload in re.findall(r'I\s*\((\d+)\).*H2_WIFI_CASE (\{[^\n]+\})', dut):
        row = json.loads(payload)
        assert row['id'] not in cases, 'duplicate case marker'
        cases[row['id']] = (int(ms) - int(row['elapsed_ms']), int(ms), row['status'])
    needed = ('access-point-real-client', 'access-point-client-left',
              'access-point-open', 'access-point-hidden')
    assert all(name in cases and cases[name][2] == 'PASS' for name in needed)
    assert cases['access-point-real-client'][0] - 3000 <= addresses[0][0] <= cases['access-point-real-client'][1] + 3000
    for i, name in ((1, 'access-point-open'), (2, 'access-point-hidden')):
        assert cases[name][0] - 3000 <= addresses[i][0] <= cases[name][1] + 3000

    clients = [(int(ms), name, int(ip), observed_mac) for ms, name, ip, observed_mac in
               re.findall(r'I\s*\((\d+)\).*H2_WIFI_FIXTURE_CLIENT target=([^\s]+) joined=\d+ ip4=(\d+) mac=([0-9a-f]{12})', fixture)]
    assert repeats in (1, 2)
    count = 3 * repeats
    for start in range(len(clients) - count + 1):
        chosen = clients[start:start + count]
        if any(name != target or observed_mac != mac or ip != addresses[i // repeats][1]
               for i, (_, name, ip, observed_mac) in enumerate(chosen)):
            continue
        offset = addresses[0][0] - chosen[0][0]
        if any(abs(chosen[i * repeats][0] + offset - addresses[i][0]) > 3000
               for i in range(3)):
            continue
        positions = ((1, 'access-point-client-left'), (3, 'access-point-open'),
                     (5, 'access-point-hidden')) if repeats == 2 else (
                         (0, 'access-point-real-client'), (1, 'access-point-open'),
                         (2, 'access-point-hidden'))
        if any(not (cases[name][0] - 3000 <= chosen[i][0] + offset <= cases[name][1] + 3000)
               for i, name in positions):
            continue
        return [(name, observed_mac, ip) for _, name, ip, observed_mac in chosen]
    raise AssertionError(f'{count} fresh fixture leases do not match DUT mode windows')
