"""Validate actual Net/TLS case reports, peer observations and boot ledgers."""
import hashlib
from pathlib import Path
import re
from datetime import datetime

ROOT = Path(__file__).resolve().parents[4]
TLS_VERIFY = -int(re.search(r'H2_PAL_ERR_TLS_VERIFY\s*=\s*-(\d+)',
    (ROOT / 'libs/pal/include/h2/pal/core/h2_pal_errors.h').read_text()).group(1))
UNSUPPORTED = -3


def check_board_observation(receipt):
    """Validate typed observed states; raw serial logs stay local by policy."""
    assert receipt['observation_contract'] == 2
    baseline = receipt['observed_baseline']
    base_status, base_dump = baseline['status'], baseline['coredump']
    execution_board = receipt.get('execution_board', receipt['platform'])
    if receipt['platform'] == 'esp32s3':
        assert execution_board in ('devkit', 'tiga_esp_v4_2', 'zero_esp_v3_0')
        assert base_status['board'] == execution_board and base_status['target'] == 'esp32s3'
    assert base_status['device_uid'] == receipt['uid']
    for value in baseline['local_source_sha256'].values():
        assert re.fullmatch('[0-9a-f]{64}', value)
    for boot in receipt['boots']:
        status, dump = boot['observed_status'], boot['observed_coredump']
        assert status['device_uid'] == receipt['uid'] and status['board'] == base_status['board']
        assert status['active_role'] == 'app' and status['active_version'] == receipt['version']
        assert status['active_checksum'] == status['partition_2_image_checksum'] == receipt['image_sha256']
        assert status['partition_2_package_checksum'] == receipt['package_sha256']
        assert status['running_partition'] == status['next_partition'] == '2'
        assert status['stage_valid'] == '0' and status['boot_intent'] == 'auto' and status['last_result'] == '0'
        for field in ('partition_1_valid','partition_1_package_checksum','partition_1_image_checksum'):
            assert status[field] == base_status[field], field
        assert status['partition_1_valid'] == '1'
        assert dump['result'] == base_dump['result'] == 'OK'
        assert dump['code'] == base_dump['code'] == '0'
        assert dump['blank'] == base_dump['blank'] and dump['stored_bytes'] == base_dump['stored_bytes']
        assert dump['partition'] == base_dump['partition'] == 'coredump'
        assert dump['bytes'] == base_dump['bytes']
        marker = boot['observed_boot']
        assert marker == dict(board=execution_board,version=receipt['version'],
            session=boot['session'],boot_id=boot['boot_id'])
        assert set(boot['local_source_sha256']) == {'serial','status','coredump_status'}
        for value in boot['local_source_sha256'].values():
            assert re.fullmatch('[0-9a-f]{64}', value)
    if base_dump['blank'] == '0':
        snapshots = receipt['observed_coredump_bytes']
        assert set(snapshots) == {'baseline','install','normal-reboot'}
        values = [bytes.fromhex(value) for value in snapshots.values()]
        assert len(values[0]) == int(base_dump['stored_bytes']) > 0
        assert all(value == values[0] for value in values)
        assert hashlib.sha256(values[0]).hexdigest() == receipt['baseline_coredump_sha256']
    else:
        assert base_dump['blank'] == '1' and base_dump['stored_bytes'] == '0'
        assert receipt['observed_coredump_bytes'] is None and receipt['baseline_coredump_sha256'] is None


def check_cases(receipt, registry):
    rows = receipt['cases']
    expected = [name for name, _ in registry]
    assert [row['id'] for row in rows] == expected, 'ledger order/count mismatch'
    summary = receipt.get('summary', receipt)
    assert summary.get('core_qualified') is True
    assert summary.get('full_net_qualified') is False
    assert summary['mandatory_passed'] == sum(int(required) for _, required in registry)
    assert summary.get('not_assessed', 0) == 0, 'supported capability was not assessed'
    assert summary['passed'] == sum(row['status'] == 'PASS' for row in rows)
    assert summary['unsupported'] == sum(row['status'] == 'UNSUPPORTED' for row in rows)
    assert not any(summary[key] for key in ('failed','blocked','retained_sockets','retained_resolvers','retained_allocations','rc','teardown'))
    for row, (_, mandatory) in zip(rows, registry):
        assert row['mandatory'] == bool(int(mandatory))
        if int(mandatory):
            assert row['status'] == 'PASS' and row['detail'] == 0, row
        else:
            assert row['status'] in ('PASS','UNSUPPORTED'), row
            if row['status'] == 'UNSUPPORTED':
                assert row['detail'] == UNSUPPORTED, row
    by_id = {row['id']: row for row in rows}
    for case in ('tls-default-untrusted','tls-wrong-ca','tls-wrong-name','tls-expired'):
        assert by_id[case]['provider_result'] == TLS_VERIFY, case
    dns_expected=[int(value) for value in receipt['dns']['operator_ipv4'].split('.')]
    assert len(dns_expected)==4 and all(0<=part<=255 for part in dns_expected)
    observed=by_id['dns-hostname']['observed_ipv4']
    assert len(observed)==4 and any(observed) and all(0<=part<=255 for part in observed), 'provider did not resolve a usable hostname address'
    assert observed == dns_expected, 'provider DNS result differs from the independent operator expectation'
    observation = receipt['dns']['observation']
    assert observation['hostname'] == receipt['dns']['host']
    assert observation['ipv4'] == receipt['dns']['operator_ipv4']
    assert observation['resolver'] and observation['method']
    assert datetime.fromisoformat(observation['observed_at_utc']).utcoffset().total_seconds() == 0
    for case in ('tls-required','tls-default','tls-sni-alpn','tls-borrowed-config'):
        assert by_id[case]['bytes_sent'] == 4193 and by_id[case]['bytes_received'] == 4097, case


def check_peer(peer, session, run_id):
    assert peer['session'] == session and re.fullmatch('[0-9a-f]{32}', session)
    assert re.fullmatch('[0-9a-f]{16}', run_id)
    rows = [row for row in peer['records'] if row.get('run_id') == run_id]
    for case in ('tls-default-untrusted','tls-wrong-ca','tls-wrong-name','tls-expired'):
        events = [row for row in rows if row['case'] == case]
        assert len(events) == 1, (case, len(events))
        event = events[0]
        assert event['session'] == session and event['accepted'] and event['client_hello'] and event['certificate_presented']
        assert event['finished'] and event['payload_received'] == 0 and event['payload_sent'] == 0, case
        assert not event['handshake_succeeded'] or event.get('post_handshake_zero_payload_close') is True, case
    tcp_payload = (
        'tcp-echo','tcp-source-bind','tcp-connect-retry',
        'tcp-listen-accept','tcp-accept-timeout','tcp-partial-io',
        'tls-required','tls-default','tls-insecure-test-only',
        'tls-sni-alpn','tls-borrowed-config','tls-repeat-wrap',
    )
    for case in tcp_payload + ('tls-session-churn',):
        events = [row for row in rows if row['case'] == case]
        assert len(events) == (12 if case == 'tls-session-churn' else 1), case
        for event in events:
            assert event['accepted'] and event['payload_valid'], case
            assert event['payload_received'] == 4193 and event['payload_sent'] == 4097, case
            if case.startswith('tls-'):
                assert event['client_hello'] and event['certificate_presented'] and event['handshake_succeeded'], case
    for case in ('udp-echo','udp-source-bind','udp-truncation'):
        events = [row for row in rows if row['case'] == case]
        assert len(events) == 1, case
        event=events[0]
        assert event['accepted'] and event['payload_valid'], case
        assert event['payload_received'] == 609 and event['payload_sent'] == 513, case
    recovery=[row for row in rows if row['case']=='tls-failure-recovery']
    assert len(recovery)==2 and recovery[0]['client_hello'] and recovery[0]['certificate_presented']
    assert recovery[0]['payload_received']==0 and not recovery[0]['handshake_succeeded']
    assert recovery[1]['payload_valid'] and recovery[1]['payload_received']==4193 and recovery[1]['payload_sent']==4097
    negotiated = [row for row in rows if row['case'] == 'tls-sni-alpn']
    assert len(negotiated) == 1 and negotiated[0]['sni'] == 'pal-net-tls.test' and negotiated[0]['alpn'] == 'h2-pal-e2e'


def check_browser(receipt, slots):
    """Validate observed unsupported operations without declaring core PASS."""
    assert receipt['platform'] == 'wasm'
    assert receipt['worker'] == 1 and receipt['main_runtime_thread'] == 0
    assert receipt['core_qualified'] is False and receipt['full_net_qualified'] is False
    assert receipt['operations'] == len(slots) == 21
    assert receipt['unexpected_results'] == receipt['teardown'] == 0
    assert receipt['cross_origin_isolated'] is True
    assert re.fullmatch('[0-9a-f]{64}', receipt['artifact_sha256'])
    assert re.fullmatch('[0-9a-f]{64}', receipt['browser_sha256'])
    rows = receipt['capabilities']
    assert len(rows) == len(slots) and {row['slot'] for row in rows} == set(slots)
    for row in rows:
        if row['slot'] in ('close', 'resolve_close'):
            assert row['status'] == 'UNSUPPORTED_NOOP'
            assert row['result'] is None and row['owned_handle'] is False
        else:
            assert row['status'] == 'UNSUPPORTED' and row['result'] == UNSUPPORTED, row
