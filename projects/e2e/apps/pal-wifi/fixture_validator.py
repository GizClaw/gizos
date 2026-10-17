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


def _timed_rows(text, markers, allow_result_clock):
    """Read every relevant row; missing/ambiguous clocks cannot disappear."""
    source = None
    previous = None
    body = '(' + '|'.join(markers) + r') .+'
    for line in text.splitlines():
        if not any(marker in line for marker in markers):
            continue
        line = re.sub(r'\x1b\[[0-9;]*m', '', line)
        assert sum(line.count(marker) for marker in markers) == 1, 'duplicate witness marker'
        explicit = re.fullmatch(r'H2_E2E_RESULT source=pal_monotonic_ms time_ms=([0-9]+) (' + body + ')', line)
        sdk = re.fullmatch(r'(?:I\s*\(([0-9]+)\)\s+pal-wifi:\s*|ap0:pal-wifi:I\(([0-9]+)\):\s*)(' + body + ')', line)
        if explicit is not None:
            assert allow_result_clock, 'peer requires its actual SDK clock'
            clock, stamp, payload = 'pal_monotonic_ms', int(explicit[1]), explicit[2]
        else:
            assert sdk is not None, 'missing or malformed device clock/witness'
            clock, stamp, payload = 'sdk', int(sdk[1] or sdk[2]), sdk[3]
        assert 0 <= stamp <= (1 << 64) - 1, 'device clock outside its integer range'
        assert source is None or source == clock, 'mixed DUT witness clocks'
        assert previous is None or stamp >= previous, 'device clock moved backwards'
        source, previous = clock, stamp
        yield stamp, payload


def _unique_object(items):
    value = {}
    for key, item in items:
        assert key not in value, 'duplicate witness JSON field'
        value[key] = item
    return value


def verify_peer_witnesses(dut_log, fixture_log, target, mac, repeats=2):
    """Match one or two real peers per AP mode by independent clock offset.

    The first DUT lease and the first fixture CLIENT establish the offset.
    WPA2/open/hidden ADDRESS markers and the remaining CLIENT times must then
    agree with that one offset and their ordered case windows. Two peers per
    mode is mandatory for final images; one supports a source-bound R31 seed.
    """
    dut = Path(dut_log).read_text(encoding='utf-8', errors='replace')
    fixture = Path(fixture_log).read_text(encoding='utf-8', errors='replace')
    addresses = []
    cases = {}
    for ms, payload in _timed_rows(dut, ('H2_WIFI_CASE', 'H2_WIFI_ADDRESS'), True):
        if payload.startswith('H2_WIFI_ADDRESS '):
            address = re.fullmatch(r'H2_WIFI_ADDRESS lease=([0-9]+) ap=([0-9]+) mask=([0-9]+)', payload)
            assert address is not None, 'malformed DUT address witness'
            observed = (ms, *(int(item) for item in address.groups()))
            assert observed not in addresses, 'duplicate DUT address witness'
            addresses.append(observed)
        else:
            row = json.loads(payload[len('H2_WIFI_CASE '):], object_pairs_hook=_unique_object)
            assert row['id'] not in cases, 'duplicate case marker'
            assert type(row['elapsed_ms']) is int and 0 <= row['elapsed_ms'] <= ms, 'invalid case elapsed time'
            cases[row['id']] = (ms - row['elapsed_ms'], ms, row['status'])
    assert len(addresses) == 3, 'missing WPA2/open/hidden lease observations'
    for _, ip, ap, mask in addresses:
        assert mask and (ip & mask) == (ap & mask), 'DUT lease outside AP subnet'
    needed = ('access-point-real-client', 'access-point-client-left',
              'access-point-open', 'access-point-hidden')
    assert all(name in cases and cases[name][2] == 'PASS' for name in needed)
    assert cases['access-point-real-client'][0] - 3000 <= addresses[0][0] <= cases['access-point-real-client'][1] + 3000
    for i, name in ((1, 'access-point-open'), (2, 'access-point-hidden')):
        assert cases[name][0] - 3000 <= addresses[i][0] <= cases[name][1] + 3000

    clients = []
    joins = set()
    for ms, payload in _timed_rows(fixture, ('H2_WIFI_FIXTURE_CLIENT',), False):
        client = re.fullmatch(r'H2_WIFI_FIXTURE_CLIENT target=([^\s]+) joined=([0-9]+) ip4=([0-9]+) mac=([0-9a-f]{12})', payload)
        assert client is not None, 'malformed peer client witness'
        name, joined, ip, observed_mac = client.groups()
        assert int(joined) not in joins, 'duplicate peer lease witness'
        joins.add(int(joined))
        clients.append((ms, name, int(ip), observed_mac))
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
