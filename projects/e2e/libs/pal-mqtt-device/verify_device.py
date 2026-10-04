"""Strict same-boot MQTT ledger and directed H2Loader preservation proof.

This verifier consumes the root task's actual serial/status/dump observations.
It never opens, scans, stages, resets or flashes a device itself.
"""
import argparse
from datetime import datetime, timedelta
import hashlib
import io
import json
import os
from pathlib import Path
import re
import signal
import tarfile
import zlib

STARTUP_MARKER = r'H2_\w*(?:BOOT|STARTUP)(?:\b|_[A-Z_]+\b)|\bBooting\b|^ESP-ROM:|^rst:0x[0-9a-f]+'

def uart_text(path):
    # Keep the authoritative bytes for receipt hashes. SDK startup may emit
    # native non-UTF8 noise around the ASCII protocol records.
    return Path(path).read_bytes().decode('utf-8', errors='replace')

def fields(text):
    return dict(re.findall(r'(\w+)=([^\s]+)', text))

def loader_status(text):
    """Read the authoritative device line, independently of host exit status."""
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    rows = re.findall(r'(?m)(?:^|\s)H2_LOADER_STATUS\s+([^\r\n]+)', text)
    assert len(rows) == 1, 'missing or ambiguous H2_LOADER_STATUS observation'
    assert '\ufffd' not in rows[0], 'corrupt device status record'
    pairs = re.findall(r'(\w+)=([^\s]+)', rows[0])
    assert pairs and len({key for key, _ in pairs}) == len(pairs), 'duplicate device status field'
    return dict(pairs)

def utc_time(value):
    timestamp = datetime.fromisoformat(value.replace('Z', '+00:00'))
    assert timestamp.tzinfo is not None and timestamp.utcoffset() == timedelta(0), 'host command requires explicit UTC time'
    return timestamp

def command_receipt(log_path, expected_command, port, controlled_capture=False):
    """Verify actual root-owned command/exit/time and bind its stdout bytes."""
    log_path = Path(log_path)
    receipt_path = log_path.with_name(log_path.stem + '-receipt.json')
    receipt = json.loads(receipt_path.read_text())
    if controlled_capture:
        assert receipt.get('controlled_stop') is True, 'monitor did not stop after validated capture'
        assert receipt.get('stop_reason') == 'validated complete ledger', 'monitor stopped for another reason'
        code = receipt.get('exit_after_capture')
        assert type(code) is int and code in (0, 130, -signal.SIGINT), 'monitor exit was not controlled SIGINT/success'
        if 'exit' in receipt:
            assert type(receipt['exit']) is int and receipt['exit'] == code, 'conflicting actual host exits'
        assert utc_time(receipt.get('captured_at_utc', '')) >= utc_time(receipt.get('started_at_utc', '')), 'capture time precedes command'
    else:
        assert type(receipt.get('exit')) is int and receipt['exit'] == 0, 'host command did not exit successfully'
    assert receipt.get('command') == expected_command, 'host receipt belongs to another command'
    assert receipt.get('port') == port, 'host command port differs from directed fixture'
    utc_time(receipt.get('started_at_utc', ''))
    assert receipt.get('log_sha256') == hashlib.sha256(log_path.read_bytes()).hexdigest(), 'host receipt does not bind actual stdout'
    return receipt

def monitor_receipt(log_path, target, port):
    """Accept only the actual supported UART observation command argv."""
    path = Path(log_path)
    actual = json.loads(path.with_name(path.stem + '-receipt.json').read_text())['command']
    base = ['reboot', target, '--monitor']
    assert actual in (base, base + ['--continuous-monitor']), 'unsupported monitor command argv'
    return command_receipt(path, actual, port, controlled_capture=True)

def after_accepted_reboot(text, target):
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    # UART command responses can interrupt an App's in-flight log fragment.
    # Keep the exact marker boundary, unique ACK and subsequent fresh boot gate.
    markers = list(re.finditer(r'(?<!\w)H2_LOADER_REBOOT\s+([^\r\n]+)', text))
    assert len(markers) == 1, 'missing or ambiguous actual reboot response'
    assert '\ufffd' not in markers[0].group(1), 'corrupt accepted reboot record'
    reboot = fields(markers[0].group(1))
    assert reboot.get('target') == target and reboot.get('result') == 'accepted', 'requested reboot not accepted'
    remainder = text[markers[0].end():]
    # Serial buffers may still carry the old App's partial replay after its ACK.
    # Only the subsequent actual startup can begin the requested boot ledger.
    startup = re.search(STARTUP_MARKER, remainder, re.MULTILINE)
    assert startup is not None, 'no actual startup after accepted reboot'
    return remainder[startup.start():]

def boot_ledger(text, ids, version, previous=None):
    boot = run = summary = accepted = None
    rows = []
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    for line in text.splitlines():
        if 'H2_PAL_MQTT_' in line:
            record = line.split('H2_PAL_MQTT_', 1)[1]
            assert '\ufffd' not in record, 'corrupt MQTT protocol record'
        if re.search(STARTUP_MARKER, line):
            boot = run = summary = accepted = None
            rows = []
            if 'H2_PAL_MQTT_BOOT ' in line:
                boot = fields(line.split('H2_PAL_MQTT_BOOT ', 1)[1])
                if boot.get('version') != version:
                    boot = None
                elif not re.fullmatch(r'[0-9a-f]{32}-[0-9a-f]{16}', boot.get('id', '')):
                    raise AssertionError('invalid fresh execution nonce')
            continue
        if 'H2_PAL_MQTT_RUN ' in line:
            assert run is None, 'later replay cannot replace the first ledger'
            run = fields(line.split('H2_PAL_MQTT_RUN ', 1)[1]); rows = []; summary = accepted = None
            assert boot is not None and run == boot, 'replay does not belong to latest complete boot identity'
            assert run['id'] != previous, 'previous boot receipt replayed'
        elif 'H2_PAL_MQTT_CASE ' in line:
            assert run is not None, 'case outside fresh execution'
            row = json.loads(line.split('H2_PAL_MQTT_CASE ', 1)[1])
            assert row['status'] == 'PASS' and row['detail'] == 0, 'case failed or blocked'
            rows.append(row)
        elif 'H2_PAL_MQTT_SUMMARY ' in line:
            assert run is not None, 'summary outside fresh execution'
            summary = json.loads(line.split('H2_PAL_MQTT_SUMMARY ', 1)[1])
            assert [row['id'] for row in rows] == ids, 'missing/duplicate/unordered mandatory ledger'
            for key, value in dict(selected=36, passed=36, failed=0, blocked=0, cleanup=0, rc=0).items():
                assert summary.get(key) == value, ('summary', key)
            assert len(summary['before']) == 10 and summary['before'] == summary['after'], 'native resource leak'
        elif 'H2_PAL_MQTT_READY ' in line:
            ready = fields(line.split('H2_PAL_MQTT_READY ', 1)[1])
            assert ready.get('rc') == '0' and ready.get('confirm') in ('0', 'pending'), 'not admitted'
            if ready.get('board') == 'devkit':
                assert ready.get('provider_cleanup') == '0', 'ESP portable provider not released'
            assert summary is not None, 'ready without latest complete ledger'
            assert accepted is None, 'duplicate terminal ledger'
            accepted = dict(boot=boot, cases=rows.copy(), summary=summary, ready=ready)
        elif 'H2_PAL_MQTT_CONFIRMED ' in line:
            assert accepted is not None and 'confirmation' not in accepted, 'duplicate or premature confirmation'
            confirmation = fields(line.split('H2_PAL_MQTT_CONFIRMED ', 1)[1])
            assert accepted is not None and accepted['ready'].get('confirm') == 'pending', 'confirmation without delivered READY'
            assert accepted['ready'].get('board') == confirmation.get('board') == 'bk7258' and confirmation.get('rc') == '0', 'app confirmation failed'
            accepted['confirmation'] = confirmation
        if re.search(r'panic|hard fault|assert failed|H2_PAL_MQTT_SETUP_FAIL', line, re.I):
            raise AssertionError('boot/runtime failure')
    assert accepted is not None, 'latest boot/replay has no complete fresh MQTT qualification'
    assert accepted['ready'].get('confirm') != 'pending' or 'confirmation' in accepted, 'missing post-READY confirmation'
    return accepted

def package_manifest(package):
    original = Path(package).read_bytes()
    with tarfile.open(fileobj=io.BytesIO(zlib.decompress(original))) as archive:
        manifest = dict(line.split('=', 1) for line in archive.extractfile('manifest').read().decode().splitlines() if line)
        members = [member for member in archive.getmembers() if member.name.startswith('app/') and member.isfile()]
        assert len(members) == 1 and manifest['role'] == 'app'
        assert (manifest['board'], manifest['target']) in (('bk7258_v3_202405', 'bk7258'), ('devkit', 'esp32s3')), 'unsupported device package'
        image = archive.extractfile(members[0]).read()
        assert len(image) == int(manifest['image_size']) and hashlib.sha256(image).hexdigest() == manifest['image_sha256']
    return manifest, hashlib.sha256(original).hexdigest()

def status_preserved(before, after, manifest, package_sha, uid):
    assert before.get('device_uid') == after.get('device_uid') == uid, 'fresh UID mismatch'
    assert before.get('board') == after.get('board') == manifest['board']
    assert before.get('target') == after.get('target') == manifest['target']
    assert before.get('partition_1_valid') == after.get('partition_1_valid') == '1'
    assert before.get('partition_1_role') == after.get('partition_1_role') == 'loader'
    for key in ('partition_1_package_checksum', 'partition_1_image_checksum'):
        assert re.fullmatch('[0-9a-f]{64}', before.get(key, '')), 'missing original valid Loader hash'
    original = {key:value for key,value in before.items() if key.startswith('partition_1_')}
    assert original and all(after.get(key) == value for key,value in original.items()), 'original Loader changed'
    for key,value in dict(active_role='app', running_partition='2', next_partition='2', stage_valid='0', last_result='0',
        active_version=manifest['version'], active_checksum=manifest['image_sha256'], partition_2_valid='1', partition_2_role='app',
        partition_2_version=manifest['version'], partition_2_image_checksum=manifest['image_sha256'], partition_2_package_checksum=package_sha).items():
        assert after.get(key) == value, ('final metadata', key, after.get(key))

def verify_witness(receipt, execution):
    boot = execution['boot']; run = receipt['runs'].get(boot['id'])
    assert run and run.get('verified') is True and run.get('session') == boot['id'], 'missing exact boot peer proof'
    assert run.get('active_clients') == run.get('retained_messages') == 0, 'broker resources retained'
    assert receipt['inputs']['ca_sha256'] == boot['ca_sha256'] and str(receipt['inputs']['epoch_ms']) == boot['epoch_ms']
    rejected = [event for event in run['tls_handshakes'] if not event['succeeded']]
    assert len(rejected) == 2 and all(event['run'] == boot['id'] and event['finished'] and event['client_hello'] and
                                    event['certificate_presented'] for event in rejected), 'TLS rejection proof crossed boot identity'
    reasons = {'TLSV1_ALERT_UNKNOWN_CA': 'tls-untrusted', 'SSLV3_ALERT_BAD_CERTIFICATE': 'tls-wrong-name'}
    observed = {}
    for event in rejected:
        case = reasons.get(event.get('error', {}).get('reason'))
        assert case and case not in observed, 'missing or duplicate distinct TLS rejection reason'
        assert 'server_name' in event, 'missing actual TLS server-name observation'
        if case == 'tls-wrong-name':
            assert event['server_name'] == 'wrong-name.invalid', 'wrong-name rejection belongs to another handshake'
        else:
            assert event['server_name'] in (None, 'localhost', receipt['inputs']['advertised']), 'untrusted CA rejection used wrong hostname'
        row = next((row for row in execution['cases'] if row['id'] == case), None)
        assert row and row['status'] == 'PASS' and row['detail'] == 0, 'TLS handshake has no matching device assertion'
        observed[case] = event
    assert set(observed) == {'tls-untrusted', 'tls-wrong-name'}, 'missing distinct TLS rejection cases'


def coredump_preserved(before, after, original=None, current=None):
    assert before == after and before.get('result') == 'OK' and before.get('code') == '0'
    stored = int(before['stored_bytes'])
    assert stored >= 0, 'invalid stored dump length'
    if stored:
        assert before['blank'] == '0' and original is not None and current is not None
        assert len(original) == len(current) == stored and original == current, 'actual coredump truncated/changed'
        return hashlib.sha256(current).hexdigest()
    assert before['blank'] == '1'
    return None

def package_binding(binding, manifest, package_sha, inputs):
    assert binding.get('package_sha256') == package_sha, 'artifact binding belongs to another package'
    assert {key:str(value) for key,value in binding['manifest'].items()} == manifest, 'artifact manifest differs from execution inputs'
    assert binding['fixture_inputs'] == inputs, 'fixture inputs differ from installed package binding'
    assert re.fullmatch('[0-9a-f]{40}', binding.get('source_commit', '')), 'missing actual artifact source'
    assert binding.get('source_dirty') is False, 'artifact source not fixed before build'

def load_package_binding(directory, manifest, package_sha, inputs):
    path = Path(directory)/'package-binding.json'
    assert path.is_file(), 'device execution requires its fixed-source package-binding.json'
    binding=json.loads(path.read_text())
    package_binding(binding,manifest,package_sha,inputs)
    return binding

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--package', type=Path, default=os.environ.get('H2_MQTT_DEVICE_PACKAGE'))
    parser.add_argument('--registry', required=True, type=Path)
    parser.add_argument('--expected-target', choices=('bk7258','esp32s3'))
    parser.add_argument('--build-label')
    args=parser.parse_args()
    if args.package is None:parser.error('explicit actually installed immutable --package or H2_MQTT_DEVICE_PACKAGE required')
    directory=Path(os.environ['H2_MQTT_DEVICE_EVIDENCE_DIR'])
    uid=os.environ.get('H2_MQTT_DEVICE_UID');port=os.environ.get('H2_MQTT_DEVICE_PORT')
    if not uid or not port:raise ValueError('explicit MQTT fixture UID/port required')
    manifest,sha=package_manifest(args.package)
    if args.expected_target:assert manifest['target']==args.expected_target, 'package belongs to another device entry'
    ids=re.findall(r'H2_PAL_MQTT_CASE\(\w+, "([^"]+)"\)',args.registry.read_text())
    assert len(ids)==36 and len(set(ids))==36
    commands={
        'managed':monitor_receipt(directory/'managed.log','upgrade',port),
        'normal':monitor_receipt(directory/'normal.log','app',port),
    }
    first=boot_ledger(after_accepted_reboot(uart_text(directory/'managed.log'),'upgrade'),ids,manifest['version'])
    second=boot_ledger(after_accepted_reboot(uart_text(directory/'normal.log'),'app'),ids,manifest['version'],first['boot']['id'])
    expected_board = 'devkit' if manifest['target'] == 'esp32s3' else 'bk7258'
    assert first['ready'].get('board') == second['ready'].get('board') == expected_board, 'ledger belongs to another board'
    for name,command in [('before-status',['status']),('after-status',['status']),
                         ('before-coredump-status',['coredump','status']),('after-coredump-status',['coredump','status'])]:
        commands[name]=command_receipt(directory/(name+'.log'),command,port)
    assert utc_time(commands['before-status']['started_at_utc']) <= utc_time(commands['managed']['started_at_utc']) <= \
        utc_time(commands['managed']['captured_at_utc']) <= utc_time(commands['normal']['started_at_utc']) <= \
        utc_time(commands['normal']['captured_at_utc']) <= utc_time(commands['after-status']['started_at_utc']), 'command observation order moved backwards'
    before=loader_status(uart_text(directory/'before-status.log'));after=loader_status(uart_text(directory/'after-status.log'))
    status_preserved(before,after,manifest,sha,uid)
    dumped_before=fields(uart_text(directory/'before-coredump-status.log'))
    dumped_after=fields(uart_text(directory/'after-coredump-status.log'))
    original=current=None
    if int(dumped_before['stored_bytes'])>0:
        original=(directory/'coredump-before.bin').read_bytes();current=(directory/'coredump-after.bin').read_bytes()
    dump_sha=coredump_preserved(dumped_before,dumped_after,original,current)
    peer=json.loads((directory/'fixture-receipt.json').read_text())
    for execution in (first,second):verify_witness(peer,execution)
    binding=load_package_binding(directory,manifest,sha,peer['inputs'])
    report=dict(manifest=manifest,package_sha256=sha,uid=uid,port=port,managed=first,normal=second,
        before_status=before,after_status=after,coredump_status=dumped_after,coredump_sha256=dump_sha,
        fixture_inputs=peer['inputs'],commands=commands,registry_sha256=hashlib.sha256(args.registry.read_bytes()).hexdigest(),
        artifact_binding=binding,build_label=args.build_label,host_verifier_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    output=Path(os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR',directory));output.mkdir(parents=True,exist_ok=True)
    (output/'qualification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(manifest['board'] + ' MQTT: 36/36 on two fresh boots; UID/P1/P2/Stage/coredump and exact peer proof preserved')
if __name__=='__main__':main()
