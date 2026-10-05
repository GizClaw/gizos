"""Reject partial/stale Net/TLS profiles and false TLS-verification receipts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from datetime import datetime

ROOT = Path(__file__).resolve().parents[4]
APP = Path(__file__).resolve().parent
PLATFORMS = {'macos', 'wasm', 'ios', 'android', 'esp32s3', 'bk7258'}
TLS_VERIFY = -int(re.search(r'H2_PAL_ERR_TLS_VERIFY\s*=\s*-(\d+)',
    (ROOT / 'libs/pal/include/h2/pal/core/h2_pal_errors.h').read_text()).group(1))
UNSUPPORTED = -3
REQUIRED_PROVIDER_SOURCES = {
    'native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net.c',
    'native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net_tls_verify.c',
    'native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net_tls_verify.h',
    'native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_net.c',
    'libs/pal/providers/posix/pal_core/src/h2_posix_net.c',
    'libs/pal/providers/ios/pal_core/src/h2_ios_net.c',
    'libs/pal/providers/ios/pal_core/include/h2_ios_net.h',
    'libs/pal/providers/ios/pal_core/BUILD.bazel',
    'libs/pal/providers/android/pal_core/src/h2_android_net.c',
    'libs/pal/providers/android/pal_core/include/h2_android_net.h',
    'libs/pal/providers/android/pal_core/BUILD.bazel',
    'libs/pal/providers/wolfssl/src/full/user_settings.h',
    'libs/pal/include/h2/pal/net/h2_pal_net.h',
}



def net_tls_build_input(relative, content):
    """Exclude only additive MQTT ownership wiring from shared mobile BUILD.

    The remaining entire file still has to match its immutable Net/TLS hash.
    This comparison preserves historical evidence, never qualifies a new SDK.
    """
    platform = {'libs/pal/providers/ios/pal_core/BUILD.bazel': 'ios',
                'libs/pal/providers/android/pal_core/BUILD.bazel': 'android'}.get(relative)
    if platform is None:
        return content
    text = content.decode('utf-8')
    text = text.replace(', "include/h2_' + platform + '_mqtt.h"', '')
    if platform == 'ios':
        text = text.replace('        ":mqtt",\n', '')
        for rule, name in [('cc_library', 'mqtt'), ('cc_test', 'mqtt_owner_lifetime_test')]:
            pattern = r'\n' + rule + r'\(\n    name = "' + name + r'",.*?\n\)\n'
            text, count = re.subn(pattern, '', text, flags=re.DOTALL)
            assert count <= 1, 'duplicate additive MQTT owner rule'
    else:
        text = text.replace('        "src/h2_android_mqtt.c",\n', '')
        text = text.replace('        "//libs/pal/providers/coremqtt",\n', '')
    return text.encode('utf-8')

IPV6_SOURCE_UPDATES = {
    'projects/e2e/apps/pal-net-tls/app/src/h2_pal_net_tls_e2e.c',
    'projects/e2e/apps/pal-net-tls/app/include/h2_pal_net_tls_e2e.h',
    'projects/e2e/libs/pal-net-tls-fixture/fixture.py',
    'libs/pal/include/h2/pal/net/h2_pal_net.h',
    'libs/pal/providers/posix/pal_core/src/h2_posix_net.c',
    'native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net.c',
}
IPV6_BK_SOURCE = 'native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_net.c'
IPV6_SLOTS = {'resolve_all', 'resolve_start_family', 'resolve_poll_all',
              'get_host_addr_family'}
IPV6_HOST_SOURCES = {
    'libs/pal/include/h2/pal/net/h2_pal_net.h',
    'libs/pal/providers/posix/pal_core/src/h2_posix_net.c',
    'libs/pal/providers/darwin/pal_core/src/h2_darwin_netif.c',
    'libs/pal/providers/corehttp/src/h2_corehttp.c',
    'libs/pal/providers/coremqtt/src/h2_coremqtt_client.c',
    'libs/pal/providers/h2peer/src/providers/portable/agent.c',
    'libs/pal/providers/h2peer/src/providers/portable/ports.c',
    'projects/e2e/apps/pal-ipv6/app/src/h2_pal_ipv6_e2e.c',
    'projects/e2e/targets/cc_binary/pal-ipv6/main.c',
    'projects/e2e/targets/cc_binary/pal-ipv6/run_desktop.py',
    'projects/e2e/apps/pal-net-tls/app/src/h2_pal_net_tls_e2e.c',
    'projects/e2e/libs/pal-net-tls-fixture/fixture.py',
    'tools/webrtc-test-server/main.go',
    'MODULE.bazel',
}


def ipv6_source_updates(root, effective):
    """Validate current source maintenance without rebinding old hardware."""
    record = json.JSONDecoder().decode((APP / 'ipv6_source_maintenance.json').read_text())
    assert record['schema'] == 1 and record['new_physical_run_claimed'] is False
    assert record['historical_qualification_sha256'] == hashlib.sha256(
        (APP / 'qualification.json').read_bytes()).hexdigest()
    assert record['historical_https_provenance_sha256'] == hashlib.sha256(
        (APP / 'gizclaw_public_https_provenance_main_tls.json').read_bytes()).hexdigest()
    current = record['current_source_sha256']
    assert IPV6_SOURCE_UPDATES <= set(current) <= IPV6_SOURCE_UPDATES | {IPV6_BK_SOURCE}
    assert record['previous_source_sha256'] == {path: effective[path] for path in current}
    assert record['historical_physical_qualification_applies_to_current_sources'] is False
    for path, expected in current.items():
        assert re.fullmatch(r'[0-9a-f]{64}', expected)
        assert hashlib.sha256((root / path).read_bytes()).hexdigest() == expected, path
    assert set(record['new_slots']) == IPV6_SLOTS
    registry = set(re.findall(r'H2_PAL_IPV6_CASE\(\w+, "([^\"]+)"\)',
        (root / 'projects/e2e/apps/pal-ipv6/app/include/h2_pal_ipv6_cases.inc').read_text()))
    assert all(cases and set(cases) <= registry for cases in record['new_slots'].values())
    assert record['validation']['desktop_ipv6_mandatory_passed'] == 56
    assert record['validation']['desktop_net_tls_mandatory_passed'] == 37
    assert record['validation']['retained_resources'] == 0
    host = record['validation']['host_execution']
    assert host['schema'] == 1 and host['platform'] == 'macos'
    assert re.fullmatch(r'[0-9a-f]{40}', host['executed_source_commit'])
    assert host['artifact_sha256'] and set(host['source_sha256']) == IPV6_HOST_SOURCES
    for path, expected in host['source_sha256'].items():
        assert hashlib.sha256((root / path).read_bytes()).hexdigest() == expected, path
    return current


def check_sources(root, sources, followup=None):
    assert REQUIRED_PROVIDER_SOURCES <= set(sources), 'missing qualified provider/config source receipt'
    effective = dict(sources)
    if followup is not None:
        # Preserve the original device/isolated-CA receipts. Only the demonstrated
        # ESP public-bundle callback fix and shared harness preflight may
        # supersede their source hashes; this does
        # not claim that the historical physical runs executed the new provider.
        changed = 'native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net.c'
        shared_runner = 'tools/bazel/mobile_e2e.py'
        assert followup['schema'] == 1 and followup['new_physical_run_claimed'] is False
        assert set(followup['current_source_sha256']) == {changed, shared_runner}
        assert followup['previous_source_sha256'] == {
            path: sources[path] for path in (changed, shared_runner)}
        assert followup['validation']['certificate_bundle_callback_regression'] == 'PASS'
        assert followup['validation']['untrusted_and_date_failures'] == 'PASS'
        assert followup['validation']['shared_mobile_contract'] == 'PASS'
        effective.update(followup['current_source_sha256'])
        effective.update(ipv6_source_updates(root, effective))
    for relative, expected in effective.items():
        content = (root / relative).read_bytes()
        if hashlib.sha256(content).hexdigest() != expected:
            content = net_tls_build_input(relative, content)
        assert hashlib.sha256(content).hexdigest() == expected, relative


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
    followup = json.loads((app / 'gizclaw_public_https_provenance_main_tls.json').read_text())
    assert followup['historical_qualification_sha256'] == hashlib.sha256(
        (app / 'qualification.json').read_bytes()).hexdigest()
    check_sources(root, data['source_sha256'], followup)
    registry = re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)',
        (app / 'app/include/h2_pal_net_tls_cases.inc').read_text())
    assert len(registry) == 39 and sum(int(required) for _,required in registry) == 37
    slots = json.loads((app / 'app/api_coverage.json').read_text())['slots']
    header = (root / 'libs/pal/include/h2/pal/net/h2_pal_net.h').read_text()
    vtable = header.split('typedef struct h2_pal_net_vtable {', 1)[1].split('} h2_pal_net_vtable_t;', 1)[0]
    assert set(slots) | IPV6_SLOTS == set(re.findall(r'\(\*(\w+)\)', vtable))
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
        if platform in ('esp32s3','bk7258'):
            board_dir = (app / entry['evidence']).parent
            metadata = json.loads((board_dir / 'firmware.json').read_text())
            assert receipt['platform'] == platform
            manifest_board = metadata['package_manifest']['board']
            assert manifest_board == ('bk7258_v3_202405' if platform == 'bk7258' else receipt['execution_board'])
            assert metadata['assets'][0]['sha256'] == receipt['package_sha256']
            assert metadata['package_manifest']['image_sha256'] == receipt['image_sha256']
            assert metadata['package_manifest']['version'] == receipt['version']
            assert len(receipt['boots']) == 2 and {row['kind'] for row in receipt['boots']} == {'install','normal-reboot'}
            assert len({row['boot_id'] for row in receipt['boots']}) == 2
            check_board_observation(receipt)
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
    print('Net/TLS historical assessment complete:', result['assessment_complete'],
          '; IPv6 provider maintenance is separate and claims no new physical run')
