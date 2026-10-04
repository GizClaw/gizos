"""Admit exact BK R4 private evidence; never infer PASS from a phase/replay.

Default is read-only verification. --apply writes only after qualified.json,
all five raw captures, actual CLI receipts and preservation evidence validate.
Private UART logs, coredump bytes and preferences are never copied into the repo.
"""
import argparse
import copy
import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

SOURCE = 'f7cc80a524795ba42021ecdd6fddf5c50adec134'
VERSION = 'pref-bk-console-r4-native'
PACKAGE = '2e1f394b4116b5a637f2897f5a5c5f5d4bb3facbd79c09a4326d901f23654762'
IMAGE = '831a078236ec5ec7fb2cef72e7078b0e6d174dd8f929de902b30fdbca13a64ff'
UID = 'c8478ca2a87c'
PORT = '/dev/cu.usbserial-1240'
COREDUMP = 'f59f522edd1cc5bd0aab008b938dc7cdfac8a4333c34fdf763404923210a3829'
PHASES = (1, 2, 3, 4, 4)
REPO = Path(__file__).resolve().parents[5]
APP = REPO / 'projects/e2e/apps/pal-storage'


def read_json(path):
    return json.loads(Path(path).read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def utc(value):
    parsed = datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
    assert parsed.tzinfo and parsed.utcoffset() == datetime.timedelta(0), 'actual aware UTC required'
    return parsed


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def oracle():
    path = REPO / 'projects/e2e/libs/pal-storage-device/verify_device.py'
    spec = importlib.util.spec_from_file_location('bk_r4_receipt_oracle', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def one_line(path, marker):
    rows = [line[line.index(marker):] for line in Path(path).read_text(errors='replace').splitlines() if marker in line]
    assert len(rows) == 1, 'one actual command response required'
    return rows[0] + '\n'


def command_receipt(path, log, expected_command, require_finished=True, monitor=False):
    receipt = read_json(path)
    assert receipt['command'] == expected_command and receipt['port'] == PORT
    actual_exit=receipt.get('actual_exit',receipt.get('exit'))
    if monitor:
        assert receipt.get('controlled_stop') is True and receipt.get('stop_signal')=='SIGINT'
        assert actual_exit in (0,130,-2), 'non-controlled or failed monitor exit'
    else:
        assert actual_exit==0, 'non-monitor actual CLI exit must be zero'
    assert receipt['log_sha256'] == sha(log), 'raw command receipt SHA mismatch'
    started = utc(receipt['started_at_utc'])
    if require_finished: assert 'finished_at_utc' in receipt, 'new capture needs actual finished UTC'
    if 'finished_at_utc' in receipt: assert utc(receipt['finished_at_utc']) >= started
    return receipt


def validate(directory):
    directory = Path(directory)
    qualified = read_json(directory / 'qualified.json')
    assert qualified.get('qualified') is True, 'private collector must first qualify all five boots'
    for key, expected in dict(contract=2, version=VERSION, source_revision=SOURCE,
                              package_sha256=PACKAGE, image_sha256=IMAGE, uid=UID, port=PORT,
                              passed=36, failed=0, blocked=0, ledger_complete=True).items():
        assert qualified.get(key) == expected, 'exact R4 identity/result mismatch: ' + key
    assert qualified['phase_order'] == list(PHASES)
    binding = read_json(directory / 'package-binding.json')
    assert binding['source_commit'] == SOURCE and binding['source_dirty'] is False
    assert binding['package_sha256'] == PACKAGE
    manifest = binding['manifest']
    assert manifest['version'] == VERSION and manifest['image_sha256'] == IMAGE and manifest['role'] == 'app'
    package = directory / 'bk7258_v3_202405-pal-storage-bk7258.update.tar.zlib'
    assert sha(package) == PACKAGE
    build = read_json(directory / 'build-receipt.json')
    assert build['source_commit'] == SOURCE and build['actual_exit'] == 0
    assert utc(build['finished_at_utc']) >= utc(build['started_at_utc'])
    boundary = read_json(directory / 'source-boundary.json')
    assert boundary['source_commit'] == SOURCE and boundary['source_dirty'] is False
    check = oracle()
    for relative in ('projects/e2e/libs/pal-storage-device/verify_device.py','projects/e2e/apps/pal-storage/app/include/h2_pal_storage_cases.inc'):
        committed=subprocess.check_output(['git','-C',str(REPO),'show',SOURCE+':'+relative])
        assert committed==(REPO/relative).read_bytes(), 'R4 oracle/registry source boundary changed'
    portable=subprocess.check_output(['git','-C',str(REPO),'show',SOURCE+':projects/e2e/libs/pal-storage-device/src/device_runner.c'])
    assert hashlib.sha256(portable).hexdigest()=='94a7aeac8ad509f4c53cab2336483787a6742a65ae7dd4b4b621d93b51df71df'
    registry = (APP / 'app/include/h2_pal_storage_cases.inc').read_text()
    logs = [directory / f'boot-{index}-phase-{phase}.log' for index, phase in enumerate(PHASES, 1)]
    verified = check.verify_run([p.read_text(errors='replace') for p in logs], registry, VERSION)
    assert verified['cases'] == qualified['cases'] and verified['phases'] == qualified['phases'], 'private ledger differs from actual captures'
    assert qualified['raw_log_sha256'] == [sha(p) for p in logs]
    receipts = []
    previous_finish = None
    for index, (phase, log) in enumerate(zip(PHASES, logs), 1):
        command = ['reboot', 'upgrade' if index == 1 else 'app', '--monitor']
        receipt = command_receipt(directory / (log.stem + '-receipt.json'), log, command, monitor=True)
        assert receipt['phase']==phase and receipt['controlled_stop'] is True
        if receipt.get('validated') is not True:
            assert index==1 and receipt['actual_exit'] in (130,-2)
            assert receipt.get('validation_error')=='AssertionError: CLI did not finish its reboot command successfully'
            admission_path=directory/'boot-1-readonly-admission-r2.json'
            admission=read_json(admission_path)
            # The sidecar is additional evidence, never an edited command receipt.
            assert admission.get('log_sha256',admission.get('raw_log_sha256'))==sha(log)
            assert admission.get('actual_exit')==receipt['actual_exit']
            assert admission.get('controlled_stop') is True
            assert admission['firmware_source_revision']==SOURCE and admission['read_only'] is True
            assert admission['command']==command and admission['stop_signal']=='SIGINT'
            assert Path(admission['original_receipt']).resolve()==(directory/(log.stem+'-receipt.json')).resolve()
            assert admission['original_receipt_sha256']==sha(directory/(log.stem+'-receipt.json'))
            assert admission['strict_report']==verified['phases'][0]
            assert admission['strict_report']['report']['version']==VERSION
            assert admission['validated'] is True
            utc(admission['admitted_at_utc'])
            independent_receipt=Path(admission['independent_status_receipt'])
            status_log=independent_receipt.with_name(independent_receipt.name.replace('-receipt.json','.log'))
            command_receipt(independent_receipt,status_log,['status'])
            check.final_status(one_line(directory/'before-status.log','H2_LOADER_STATUS '),one_line(status_log,'H2_LOADER_STATUS '),manifest,PACKAGE,UID)
            # Raw original ACK, native success and the full strict oracle were
            # independently rechecked above; interrupt exits are not rewritten.
            assert 'H2_LOADER_REBOOT target=upgrade result=accepted' in log.read_text(errors='replace')
        else:
            assert receipt.get('validation_error') is None
        identity = receipt['identity']
        assert identity['source_revision'] == SOURCE and identity['package_sha256'] == PACKAGE and identity['image_sha256'] == IMAGE
        started, finished = utc(receipt['started_at_utc']), utc(receipt['finished_at_utc'])
        if previous_finish: assert started >= previous_finish, 'boot capture command intervals overlap'
        previous_finish = finished
        assert isinstance(receipt['host_pid'], int) and receipt['host_pid'] > 0
        public_command={key:receipt[key] for key in ('command','phase','host_pid','actual_exit','started_at_utc','finished_at_utc','controlled_stop','stop_signal','validated','log_sha256')}
        if receipt.get('validated') is not True:
            public_command.update(readonly_admission_sha256=sha(admission_path),readonly_admitted=True,
                original_validation_error=receipt['validation_error'],
                admission_boundary='Original collector wrongly rejected a normal controlled SIGINT exit after a complete strict ledger; original actual exit and receipt are retained.')
        receipts.append(public_command)
    assert len({r['host_pid'] for r in receipts}) == 5, 'five independent monitor processes required'
    before = one_line(directory / 'before-status.log', 'H2_LOADER_STATUS ')
    after = one_line(directory / 'after-status.log', 'H2_LOADER_STATUS ')
    command_receipt(directory / 'before-status-receipt.json', directory / 'before-status.log', ['status'], require_finished=False)
    command_receipt(directory / 'after-status-receipt.json', directory / 'after-status.log', ['status'])
    status = check.final_status(before, after, manifest, PACKAGE, UID)
    assert status['partition_2_role'] == 'app' and status['stage_valid'] == '0'
    assert status['active_image_size'] == status['partition_2_image_size'] == manifest['image_size']
    before_cd = one_line(directory / 'before-coredump-status.log', 'H2_LOADER_COREDUMP_STATUS ')
    after_cd = one_line(directory / 'after-coredump-status.log', 'H2_LOADER_COREDUMP_STATUS ')
    command_receipt(directory / 'before-coredump-status-receipt.json', directory / 'before-coredump-status.log', ['coredump', 'status'], require_finished=False)
    command_receipt(directory / 'after-coredump-status-receipt.json', directory / 'after-coredump-status.log', ['coredump', 'status'])
    before_dump, after_dump = directory / 'coredump-before.bin', directory / 'coredump-after.bin'
    assert len(before_dump.read_bytes()) == len(after_dump.read_bytes()) == 32
    assert before_dump.read_bytes() == after_dump.read_bytes() and sha(before_dump) == sha(after_dump) == COREDUMP
    before_dump_receipt=read_json(directory/'before-coredump-dump-receipt.json')
    command_receipt(directory/'before-coredump-dump-receipt.json',directory/'before-coredump-dump.log',before_dump_receipt['command'],require_finished=False)
    assert before_dump_receipt['command'][:3]==['coredump','dump','--output'] and Path(before_dump_receipt['command'][3]).resolve()==before_dump.resolve()
    dump_receipt = read_json(directory / 'after-coredump-dump-receipt.json')
    command_receipt(directory / 'after-coredump-dump-receipt.json', directory / 'after-coredump-dump.log', dump_receipt['command'])
    assert dump_receipt['command'][:3] == ['coredump', 'dump', '--output']
    assert Path(dump_receipt['command'][3]).resolve() == after_dump.resolve()
    dump_fields = check.fields(one_line(directory / 'after-coredump-dump.log', 'H2_LOADER_COREDUMP_FILE '))
    assert dump_fields['result'] == 'OK' and dump_fields['bytes'] == '32'
    assert Path(dump_fields['path']).resolve() == after_dump.resolve()
    cd = check.coredump_status(before_cd, after_cd, sha(before_dump), sha(after_dump))
    assert cd['blank'] == '0' and cd['stored_bytes'] == '32'
    public = copy.deepcopy(verified)
    public.update(platform='bk7258', qualified=True, status='qualified', source_revision=SOURCE,
                  source_dirty=False, artifact=dict(sha256=PACKAGE, image_sha256=IMAGE, manifest=manifest),
                  uid=UID, raw_log_sha256=[sha(p) for p in logs], reboot_receipts=receipts,
                  private_qualification_sha256=sha(directory / 'qualified.json'),
                  package_binding_sha256=sha(directory / 'package-binding.json'),
                  native_build_receipt_sha256=sha(directory/'build-receipt.json'),
                  portable_runner_sha256=hashlib.sha256(portable).hexdigest(),
                  preservation=dict(loader={key: value for key, value in status.items() if key.startswith('partition_1_')},
                    final={key: status[key] for key in ('active_role','active_version','active_checksum','running_partition','next_partition','stage_valid','partition_2_valid','partition_2_role','partition_2_version','partition_2_image_checksum','partition_2_package_checksum','last_result')},
                    coredump=dict(stored_bytes=32, blank=False, sha256=COREDUMP)),
                  host_verifier_sha256=sha(REPO / 'projects/e2e/libs/pal-storage-device/verify_device.py'),
                  recorded_at_utc=qualified['finished_at_utc'],
                  evidence_boundary='Five actual independent 1/2/3/4/4 boot command receipts (controlled SIGINT actual 0/130/-2 retained), strict 36-case/fresh/immutable-replay oracle, exact R4 artifact, unchanged P1 and original nonblank 32-byte dump, valid P2 and Stage empty. Native UART/backup values remain private; earlier source-bound failures are not rebound.')
    return public


def historical_capture(directory):
    directory = Path(directory)
    binding = read_json(directory / 'package-binding.json')
    receipt = read_json(directory / 'boot-1-phase-1-receipt.json')
    failure = read_json(directory / 'collector-failure.json')
    log = directory / 'boot-1-phase-1.log'
    assert failure['qualified'] is False and receipt['validated'] is False
    assert receipt['log_sha256'] == sha(log)
    text = log.read_text(errors='replace')
    boots = re.findall(r'H2_STORAGE_BOOT ([^\r\n]+)', text)
    return dict(contract=2, platform='bk7258', qualified=False, status='capture_failed',
                source_revision=binding['source_commit'], version=binding['manifest']['version'],
                artifact=dict(sha256=binding['package_sha256'], image_sha256=binding['manifest']['image_sha256']),
                observed_fresh_boots=sum('replay=1' not in row for row in boots),
                observed_replay_boots=sum('replay=1' in row for row in boots),
                actual_cli_exit=receipt['actual_exit'], raw_log_sha256=sha(log),
                recorded_at_utc=receipt['finished_at_utc'], reason='collector did not establish five independent qualified boots',
                evidence_boundary='Historical collector failure, not a new-source result and not five-boot qualification. Phase-local PASS/replay/confirmation cannot supply a missing fresh marker or the other phases.')


def apply(directory, public):
    matrix_path = APP / 'qualification-v2.json'
    matrix = read_json(matrix_path)
    old_platforms = copy.deepcopy(matrix['qualified_platforms'])
    assert len(old_platforms)==5 and {row['platform'] for row in old_platforms}=={'macos','wasm-chromium','ios-simulator','android-emulator','esp32s3-devkit'}
    assert all(row['status']=='qualified' for row in old_platforms)
    preserve = {row['receipt']: sha(APP / row['receipt']) for row in old_platforms}
    failures = copy.deepcopy(matrix.get('historical_failures', []))
    failures.extend(copy.deepcopy(matrix.get('failed_platforms', [])))
    parent = Path(directory).parent
    historical_updates=[]
    for private_folder, public_name in [('pref-bk-large-r2','bk7258-r2-capture-failed.json'),
                                        ('pref-bk-cache-r3','bk7258-r3-capture-failed.json')]:
        historical = historical_capture(parent / private_folder)
        historical_updates.append((public_name,historical))
        failures.append(dict(platform='bk7258', status='capture_failed', receipt='evidence/contract2/'+public_name,
                             source_revision=historical['source_revision'], version=historical['version']))
    for name,historical in historical_updates: write_json(APP/'evidence/contract2'/name,historical)
    write_json(APP / 'evidence/contract2/bk7258-r4-qualified.json', public)
    matrix['qualified_platforms'].append(dict(platform='bk7258', status='qualified',
        receipt='evidence/contract2/bk7258-r4-qualified.json', source_revision=SOURCE,
        version=VERSION, artifact_sha256=PACKAGE, image_sha256=IMAGE,
        host_verifier_sha256=public['host_verifier_sha256']))
    matrix['historical_failures'] = failures
    matrix['failed_platforms'] = []
    matrix['pending_platforms'] = []
    matrix['all_platforms_qualified'] = True
    matrix['firmware_builds'].append(dict(target='//projects/e2e/targets/h2loader_tar_zlib/pal-storage/bk7258_v3_202405:package',
        source_revision=SOURCE, version=VERSION, status='build_passed', hardware_qualification='passed',
        artifact_sha256=PACKAGE, image_sha256=IMAGE))
    matrix['qualification_boundary'] = 'All six platforms have their own source/artifact-bound contract 2 36-case receipt. BK R4 completes five fresh 1/2/3/4/4 boots, valid P2/Stage empty, unchanged original P1 and nonblank dump. The original de68 capacity failure and R2/R3 collector failures remain historical evidence. Mobile and other earlier source identities are unchanged; no physical-phone or power-loss guarantee is inferred.'
    matrix['updated_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    assert matrix['qualified_platforms'][:5] == old_platforms
    assert all(sha(APP / path) == value for path,value in preserve.items())
    write_json(matrix_path, matrix)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('private_directory', type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    if not (args.private_directory / 'qualified.json').exists():
        print('WAIT: R4 private qualified.json is absent; no qualification metadata changed')
        return 2
    public = validate(args.private_directory)
    if args.apply: apply(args.private_directory, public)
    print(json.dumps(dict(validated=True, metadata_applied=args.apply, source_revision=SOURCE, version=VERSION, passed=36, phases=list(PHASES))))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
