"""Reject partial/stale Net/TLS profiles and false TLS-verification receipts."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]
APP = Path(__file__).resolve().parent
PLATFORMS = {'macos', 'wasm', 'ios', 'android', 'devkit', 'bk7258'}
TLS_VERIFY = -int(re.search(r'H2_PAL_ERR_TLS_VERIFY\s*=\s*-(\d+)',
    (ROOT / 'libs/pal/include/h2/pal/core/h2_pal_errors.h').read_text()).group(1))


def check_cases(receipt, registry):
    rows = receipt['cases']
    expected = [name for name, _ in registry]
    assert [row['id'] for row in rows] == expected, 'ledger order/count mismatch'
    summary = receipt.get('summary', receipt)
    assert summary.get('core_qualified') is True
    assert summary.get('full_net_qualified') is False
    assert summary['mandatory_passed'] == sum(int(required) for _, required in registry)
    assert not any(summary[key] for key in ('failed','blocked','retained_sockets','retained_resolvers','retained_allocations','rc','teardown'))
    for row, (_, mandatory) in zip(rows, registry):
        assert row['mandatory'] == bool(int(mandatory))
        if int(mandatory):
            assert row['status'] == 'PASS' and row['detail'] == 0, row
        else:
            assert row['status'] in ('PASS','UNSUPPORTED','NOT_ASSESSED'), row
    by_id = {row['id']: row for row in rows}
    for case in ('tls-default-untrusted','tls-wrong-ca','tls-wrong-name','tls-expired'):
        assert by_id[case]['provider_result'] == TLS_VERIFY, case
    dns_expected=[int(value) for value in receipt['dns']['operator_ipv4'].split('.')]
    assert len(dns_expected)==4 and all(0<=part<=255 for part in dns_expected)
    observed=by_id['dns-hostname']['observed_ipv4']
    assert len(observed)==4 and any(observed) and all(0<=part<=255 for part in observed), 'provider did not resolve a usable hostname address'
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


def check(root=ROOT, allow_pending=False):
    app = root / 'projects/e2e/apps/pal-net-tls'
    data = json.loads((app / 'qualification.json').read_text())
    assert set(data['platforms']) == PLATFORMS and data['full_net_qualified'] is False
    if allow_pending and not data['assessment_complete']:
        assert data['pending']
        return data
    assert data['assessment_complete'] and not data['pending']
    assert data['gate']['wifi_qualification_verified'] and data['gate']['hardware_released']
    assert data['gate']['tls_integration_started']
    assert data['source_sha256']
    for relative, expected in data['source_sha256'].items():
        assert hashlib.sha256((root / relative).read_bytes()).hexdigest() == expected, relative
    registry = re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)',
        (app / 'app/include/h2_pal_net_tls_cases.inc').read_text())
    assert len(registry) == 39 and sum(int(required) for _,required in registry) == 37
    for platform, entry in data['platforms'].items():
        receipt = json.loads((app / entry['evidence']).read_text())
        assert re.fullmatch('[0-9a-f]{64}', receipt['artifact_sha256'])
        assert entry['status'] == 'PASS' and entry['core_qualified']
        if platform in ('devkit','bk7258'):
            assert len(receipt['boots']) == 2 and {row['kind'] for row in receipt['boots']} == {'install','normal-reboot'}
            assert len({row['boot_id'] for row in receipt['boots']}) == 2
            for boot in receipt['boots']:
                assert boot['uid'] == receipt['uid'] and boot['image_sha256'] == receipt['image_sha256']
                assert boot['confirm'] == 0 and boot['loader_p1_preserved'] and boot['stage_empty'] and boot['coredump_unchanged']
                check_cases(boot, registry)
                check_peer(boot['peer'], boot['session'], boot['boot_id'])
        else:
            check_cases(receipt, registry)
            check_peer(receipt['peer'], receipt['peer']['session'], receipt['peer']['session'][:16])
            if platform in ('ios','android'):
                assert re.fullmatch('[0-9a-f]{64}', receipt['sdk_sha256'])
                assert receipt['packaged_sdk_symbols_verified']
    return data


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--allow-pending',action='store_true')
    args=parser.parse_args()
    result=check(allow_pending=args.allow_pending)
    print('Net/TLS assessment complete:', result['assessment_complete'])
