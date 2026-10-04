"""Import an already qualified BK capture without UART or fixture access.

Raw native logs, storage and coredump bytes stay in the private evidence folder.
The importer reruns the exact recorded host verifier into a temporary directory,
then writes only normative JSON into a new, exclusive destination.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(Path(path).read_text())


def write(path, value):
    with Path(path).open('x') as output:
        json.dump(value, output, indent=2)
        output.write('\n')


def admitted(folder, source, version):
    collector = read(folder / 'collector-receipt.json')
    assert collector.get('qualified') is True, 'collector has not qualified this run'
    assert type(collector.get('strict_verifier_exit')) is int and collector['strict_verifier_exit'] == 0, 'strict host verifier did not exit 0'
    assert 'error' not in collector or collector['error'] is None, 'collector recorded failure'
    for name, digest in collector['logs_sha256'].items():
        assert Path(name).name == name and sha(folder / name) == digest, 'actual capture/log bytes changed'
    assert 'strict-verifier.log' in collector['logs_sha256'], 'missing actual strict invocation log'
    boundary = read(folder / 'source-boundary.json')
    assert boundary == dict(source_commit=source, source_dirty=False), 'build source was not fixed and clean'
    build = read(folder / 'build-receipt.json')
    assert type(build.get('actual_exit')) is int and build['actual_exit'] == 0 and build['source_commit'] == source, 'build did not succeed on this source'
    assert '--//tools/bazel:firmware_version=' + version in build['argv'], 'build version differs'
    binding = read(folder / 'package-binding.json')
    assert binding['source_commit'] == source and binding['source_dirty'] is False, 'artifact belongs to another source'
    assert binding['manifest']['version'] == version, 'artifact belongs to another version'
    assert collector['package_sha256'] == binding['package_sha256'], 'collector belongs to another package'
    return collector


def import_run(folder, destination, repo, source, version, uid, dump_sha):
    assert not destination.exists(), 'refusing to overwrite historical qualification'
    assert re.fullmatch('[0-9a-f]{40}', source) and re.fullmatch('[0-9a-f]{64}', dump_sha)
    collector = admitted(folder, source, version)
    verifier = repo / 'projects/e2e/libs/pal-mqtt-device/verify_device.py'
    assert sha(verifier) == collector['verifier_sha256'], 'host verifier changed since the actual qualification'
    packages = list(folder.glob('*.update.tar.zlib'))
    assert len(packages) == 1 and sha(packages[0]) == collector['package_sha256'], 'immutable installed package missing/changed'
    original = read(folder / 'qualification.json')
    assert original['artifact_binding']['source_commit'] == source and original['manifest']['version'] == version
    assert original['uid'] == uid and original['manifest']['target'] == 'bk7258'
    assert original['coredump_status']['stored_bytes'] == '32' and original['coredump_sha256'] == dump_sha, 'original actual 32-byte coredump not preserved'
    assert original['host_verifier_sha256'] == collector['verifier_sha256']
    with tempfile.TemporaryDirectory(prefix='pal-mqtt-bk-import-') as temporary:
        environment = dict(os.environ, H2_MQTT_DEVICE_EVIDENCE_DIR=str(folder),
            H2_MQTT_DEVICE_UID=uid, H2_MQTT_DEVICE_PORT=original['port'], TEST_UNDECLARED_OUTPUTS_DIR=temporary)
        result = subprocess.run([sys.executable, str(verifier), '--package', str(packages[0]),
            '--registry', str(repo / 'projects/e2e/apps/pal-mqtt/app/include/h2_pal_mqtt_cases.inc'),
            '--expected-target', 'bk7258'], env=environment, capture_output=True)
        assert result.returncode == 0, 'read-only strict revalidation failed: ' + result.stderr.decode(errors='replace')
        qualified = read(Path(temporary) / 'qualification.json')
        assert qualified == original, 'read-only proof differs from the actual recorded qualification'
    peer = read(folder / 'fixture-receipt.json')
    oracle = runpy.run_path(str(verifier), run_name='mqtt_import_verifier')
    registry = re.findall(r'H2_PAL_MQTT_CASE\(\w+, "([^"]+)"\)',
        (repo / 'projects/e2e/apps/pal-mqtt/app/include/h2_pal_mqtt_cases.inc').read_text())
    sessions = []
    for name, target in [('managed', 'upgrade'), ('normal', 'app')]:
        execution = qualified[name]
        sessions.append(execution['boot']['id'])
        row = next(row for row in execution['cases'] if row['id'] == 'retained-delivery')
        assert row['received'] == 2 and row['disconnected'] == 1, 'strengthened retained case missing'
        run = peer['runs'][execution['boot']['id']]
        wire = run['tcp_arrivals'][execution['boot']['id'] + '-retained-delivery']
        assert wire['publish'] == 2 and wire['disconnect'] == 1, 'exact retained wire lifecycle missing'
        receipt = read(folder / (name + '-receipt.json'))
        assert receipt['controlled_stop'] is True and receipt['stop_reason'] == 'validated complete ledger'
        assert receipt['command'] in (['reboot', target, '--monitor'], ['reboot', target, '--monitor', '--continuous-monitor'])
        # Check the first READY, not a later complete replay after an incomplete
        # terminal. The actual verifier already checks the whole stream/late BOOT.
        text = (folder / (name + '.log')).read_bytes().decode('utf-8', errors='replace')
        ack = re.search(r'(?<!\w)H2_LOADER_REBOOT\s+[^\r\n]+', text)
        assert ack is not None
        ready = re.search(r'(?<!\w)H2_PAL_MQTT_READY\s+[^\r\n]+[\r\n]', text[ack.end():])
        assert ready is not None
        prefix = text[:ack.end() + ready.end()]
        first = oracle['boot_ledger'](oracle['after_accepted_reboot'](prefix, target), registry,
            version, sessions[0] if name == 'normal' else None)
        assert first == execution, 'first READY differs from later accepted replay'
    assert len(set(sessions)) == 2, 'boot identity reused'
    destination.mkdir(parents=True, exist_ok=False)
    write(destination / 'qualified.json', qualified)
    for name in ['source-boundary.json', 'build-receipt.json', 'package-binding.json', 'collector-receipt.json']:
        shutil.copyfile(folder / name, destination / name)
    receipts = {path.name: read(path) for path in folder.glob('*-receipt.json') if 'command' in read(path)}
    write(destination / 'host-command-receipts.json', receipts)
    write(destination / 'peer-witness.json', dict(inputs=peer['inputs'], runs={session: peer['runs'][session] for session in sessions}))
    write(destination / 'environment.json', dict(qualified=True, restored=False,
        artifact_source_commit=source, host_verifier_sha256=collector['verifier_sha256'],
        version=version, uid=uid, case_count=36, boots=sessions,
        raw_evidence_directory=str(folder), recorded_at_utc=datetime.now(timezone.utc).isoformat(),
        note='Qualification precedes the separately recorded root-owned restoration.'))
    write(destination / 'evidence-manifest.json', {str(path.relative_to(folder)): sha(path)
        for path in folder.iterdir() if path.is_file()})
    print('Imported two fresh 36/36 BK boots with complete strict proof; restoration remains separate.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-directory', required=True, type=Path)
    parser.add_argument('--output-directory', required=True, type=Path)
    parser.add_argument('--verifier-repo', required=True, type=Path)
    parser.add_argument('--expected-source', required=True)
    parser.add_argument('--expected-version', required=True)
    parser.add_argument('--expected-uid', required=True)
    parser.add_argument('--expected-coredump-sha256', required=True)
    args = parser.parse_args()
    import_run(args.evidence_directory, args.output_directory, args.verifier_repo,
        args.expected_source, args.expected_version, args.expected_uid, args.expected_coredump_sha256)


if __name__ == '__main__':
    main()
