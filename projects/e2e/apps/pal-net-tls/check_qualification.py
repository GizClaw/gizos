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
UNSUPPORTED = -3


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


def check(root=ROOT, allow_pending=False, require_all_core=False):
    app = root / 'projects/e2e/apps/pal-net-tls'
    data = json.loads((app / 'qualification.json').read_text())
    assert set(data['platforms']) == PLATFORMS and data['full_net_qualified'] is False
    assert data['acceptance_policy'] == 'supported-pass-unsupported-skip'
    pending = set(data['pending'])
    assert pending <= PLATFORMS and len(data['pending']) == len(pending)
    if not allow_pending:
        assert data['assessment_complete'] and not pending
    else:
        assert data['assessment_complete'] == (not pending)
    assert data['gate']['wifi_qualification_verified'] and data['gate']['hardware_released']
    assert data['gate']['tls_integration_started']
    assert data['source_sha256']
    for relative, expected in data['source_sha256'].items():
        assert hashlib.sha256((root / relative).read_bytes()).hexdigest() == expected, relative
    registry = re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)',
        (app / 'app/include/h2_pal_net_tls_cases.inc').read_text())
    assert len(registry) == 39 and sum(int(required) for _,required in registry) == 37
    slots = json.loads((app / 'app/api_coverage.json').read_text())['slots']
    header = (root / 'libs/pal/include/h2/pal/net/h2_pal_net.h').read_text()
    vtable = header.split('typedef struct h2_pal_net_vtable {', 1)[1].split('} h2_pal_net_vtable_t;', 1)[0]
    assert set(slots) == set(re.findall(r'\(\*(\w+)\)', vtable))
    for platform, entry in data['platforms'].items():
        receipt = json.loads((app / entry['evidence']).read_text())
        if platform in pending:
            assert entry['status'] == 'PENDING' and not entry['core_qualified']
            continue
        assert entry['assessment_complete'] is True
        assert entry['receipt_sha256'] == hashlib.sha256((app / entry['evidence']).read_bytes()).hexdigest()
        assert entry['artifact_sha256'] == receipt['artifact_sha256']
        if platform == 'wasm':
            assert not require_all_core, 'Browser raw Net/TLS is unsupported, not core qualified'
            assert entry['status'] == 'SKIP' and entry['core_qualified'] is False
            assert entry['skip_reason'] == 'PROVIDER_UNSUPPORTED'
            assert entry['skipped_case_ids'] == [name for name, _ in registry]
            assert entry['mandatory_passed'] == 0 and entry['mandatory_skipped'] == 37
            check_browser(receipt, slots)
            continue
        assert entry['status'] == 'PASS' and entry['core_qualified']
        assert re.fullmatch('[0-9a-f]{64}', receipt['artifact_sha256'])
        assert entry['artifact_sha256'] == receipt['artifact_sha256']
        assert entry['receipt_sha256'] == hashlib.sha256((app / entry['evidence']).read_bytes()).hexdigest()
        if platform in ('devkit','bk7258'):
            board_dir = app / 'evidence' / platform
            metadata = json.loads((board_dir / 'firmware.json').read_text())
            assert metadata['assets'][0]['sha256'] == receipt['package_sha256']
            assert metadata['package_manifest']['image_sha256'] == receipt['image_sha256']
            assert metadata['package_manifest']['version'] == receipt['version']
            assert len(receipt['boots']) == 2 and {row['kind'] for row in receipt['boots']} == {'install','normal-reboot'}
            assert len({row['boot_id'] for row in receipt['boots']}) == 2
            for boot in receipt['boots']:
                log = board_dir / (boot['kind'] + '.log')
                assert boot['log_sha256'] == hashlib.sha256(log.read_bytes()).hexdigest()
                assert boot['boot_id'] in log.read_text(errors='replace')
                assert boot['uid'] == receipt['uid'] and boot['image_sha256'] == receipt['image_sha256']
                assert boot['confirm'] == 0 and boot['loader_p1_preserved'] and boot['stage_empty'] and boot['coredump_unchanged']
                check_cases(boot, registry)
                check_peer(boot['peer'], boot['session'], boot['boot_id'])
            if platform == 'bk7258':
                dump_hashes = {hashlib.sha256((board_dir / (kind + '-dump.bin')).read_bytes()).hexdigest()
                               for kind in ('baseline','install','normal')}
                assert dump_hashes == {receipt['baseline_coredump_sha256']}
            else:
                assert receipt['baseline_coredump_sha256'] is None
        else:
            check_cases(receipt, registry)
            check_peer(receipt['peer'], receipt['peer']['session'], receipt['peer']['session'][:16])
            if platform in ('ios','android'):
                assert re.fullmatch('[0-9a-f]{64}', receipt['sdk_sha256'])
                assert receipt['packaged_sdk_symbols_verified']
                environment_path = app / entry['environment']
                assert entry['environment_sha256'] == hashlib.sha256(environment_path.read_bytes()).hexdigest()
                environment = json.loads(environment_path.read_text())
                assert environment['runner_status'] == 'completed' and environment['launches'] == 1
                assert not environment.get('cleanup_errors')
                assert environment['suite_sha256'] == entry['suite_sha256']
                assert re.fullmatch('[0-9a-f]{64}', entry['suite_sha256'])
                assert environment['app_sha256'] == receipt['artifact_sha256']
                assert environment['sdk_sha256'] == receipt['sdk_sha256']
    return data


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--allow-pending',action='store_true')
    parser.add_argument('--require-all-core',action='store_true')
    args=parser.parse_args()
    result=check(allow_pending=args.allow_pending, require_all_core=args.require_all_core)
    print('Net/TLS assessment complete:', result['assessment_complete'])
