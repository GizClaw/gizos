"""Strict same-boot MQTT ledger and directed H2Loader preservation proof.

This verifier consumes the root task's actual serial/status/dump observations.
It never opens, scans, stages, resets or flashes a device itself.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tarfile
import zlib

def fields(text):
    return dict(re.findall(r'(\w+)=([^\s]+)', text))

def boot_ledger(text, ids, version, previous=None):
    boot = run = summary = accepted = None
    rows = []
    for line in text.splitlines():
        if re.search(r'H2_\w*(?:BOOT|STARTUP)\b|\bBooting\b', line):
            boot = run = summary = accepted = None
            rows = []
            if 'H2_PAL_MQTT_BOOT ' in line:
                boot = fields(line)
                if boot.get('version') != version:
                    boot = None
                elif not re.fullmatch(r'[0-9a-f]{32}-[0-9a-f]{16}', boot.get('id', '')):
                    raise AssertionError('invalid fresh execution nonce')
            continue
        if 'H2_PAL_MQTT_RUN ' in line:
            run = fields(line); rows = []; summary = accepted = None
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
            ready = fields(line)
            assert ready.get('rc') == '0' and ready.get('confirm') == '0', 'not admitted'
            assert summary is not None, 'ready without latest complete ledger'
            accepted = dict(boot=boot, cases=rows.copy(), summary=summary, ready=ready)
        if re.search(r'panic|hard fault|assert failed|H2_PAL_MQTT_SETUP_FAIL', line, re.I):
            raise AssertionError('boot/runtime failure')
    assert accepted is not None, 'latest boot/replay has no complete fresh MQTT qualification'
    return accepted

def package_manifest(package):
    original = Path(package).read_bytes()
    with tarfile.open(fileobj=io.BytesIO(zlib.decompress(original))) as archive:
        manifest = dict(line.split('=', 1) for line in archive.extractfile('manifest').read().decode().splitlines() if line)
        members = [member for member in archive.getmembers() if member.name.startswith('app/') and member.isfile()]
        assert len(members) == 1 and manifest['role'] == 'app' and manifest['target'] == 'bk7258'
        image = archive.extractfile(members[0]).read()
        assert len(image) == int(manifest['image_size']) and hashlib.sha256(image).hexdigest() == manifest['image_sha256']
    return manifest, hashlib.sha256(original).hexdigest()

def status_preserved(before, after, manifest, package_sha, uid):
    assert before.get('result') == after.get('result') == 'OK' and before.get('code') == after.get('code') == '0'
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

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--registry', required=True, type=Path)
    args=parser.parse_args()
    directory=Path(os.environ['H2_MQTT_DEVICE_EVIDENCE_DIR'])
    uid=os.environ.get('H2_MQTT_DEVICE_UID');port=os.environ.get('H2_MQTT_DEVICE_PORT')
    if not uid or not port:raise ValueError('explicit MQTT fixture UID/port required')
    manifest,sha=package_manifest(args.package)
    ids=re.findall(r'H2_PAL_MQTT_CASE\(\w+, "([^"]+)"\)',args.registry.read_text())
    assert len(ids)==36 and len(set(ids))==36
    first=boot_ledger((directory/'managed.log').read_text(),ids,manifest['version'])
    second=boot_ledger((directory/'normal.log').read_text(),ids,manifest['version'],first['boot']['id'])
    before=fields((directory/'before-status.log').read_text());after=fields((directory/'after-status.log').read_text())
    status_preserved(before,after,manifest,sha,uid)
    dumped_before=fields((directory/'before-coredump-status.log').read_text())
    dumped_after=fields((directory/'after-coredump-status.log').read_text())
    original=current=None
    if int(dumped_before['stored_bytes'])>0:
        original=(directory/'coredump-before.bin').read_bytes();current=(directory/'coredump-after.bin').read_bytes()
    dump_sha=coredump_preserved(dumped_before,dumped_after,original,current)
    peer=json.loads((directory/'fixture-receipt.json').read_text())
    for execution in (first,second):verify_witness(peer,execution)
    report=dict(manifest=manifest,package_sha256=sha,uid=uid,port=port,managed=first,normal=second,
        before_status=before,after_status=after,coredump_status=dumped_after,coredump_sha256=dump_sha,
        fixture_inputs=peer['inputs'],registry_sha256=hashlib.sha256(args.registry.read_bytes()).hexdigest())
    output=Path(os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR',directory));output.mkdir(parents=True,exist_ok=True)
    (output/'qualification.json').write_text(json.dumps(report,indent=2)+'\n')
    print('BK7258 MQTT: 36/36 on two fresh boots; UID/P1/P2/Stage/coredump and exact peer proof preserved')
if __name__=='__main__':main()
