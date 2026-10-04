"""Validate Storage contract 2 boot ledgers without replaying device commands."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def version_nonce(version):
    nonce = 2166136261
    for byte in version.encode():
        nonce = ((nonce ^ byte) * 16777619) & 0xffffffff
    return nonce


def boot_ledger(text, registry, version, phase):
    entries = re.findall(r'H2_PAL_STORAGE_CASE\("([^\"]+)", ([123])\)', registry)
    assert len(entries) == len({name for name, _ in entries}) == 36, "invalid registry"
    selected = [name for name, value in entries if int(value) == phase]
    marker = re.compile(r'H2_STORAGE_BOOT contract=2 version=' + re.escape(version) +
                        r' phase=' + str(phase) + r' nonce=(\d+)([^\r\n]*)')
    boots = [match for match in marker.finditer(text) if 'replay=1' not in match.group(2)]
    assert len(boots) == 1, "one independently captured boot marker is required"
    boot = boots[0]
    current = text[boot.end():]
    nonce = int(boot.group(1))
    assert 'H2_STORAGE_READY rc=0 confirm=0' in current, "App not confirmed or failed"
    assert not re.search(r'panic|hard fault|assert failed|H2_STORAGE_(?:LAUNCHER_FAIL|WATCHDOG)',
                         current, re.I), "boot failed"
    # Reboot monitor may precede this marker with the old App's replay. Once
    # the selected new boot begins, every BOOT must identify its same replay.
    for header in re.findall(r'H2_STORAGE_BOOT ([^\r\n]*)', current):
        replay = fields(header)
        expected = dict(contract='2', version=version, phase=str(phase), nonce=str(nonce), replay='1')
        assert all(replay.get(key) == value for key, value in expected.items()), "new boot or changed replay identity"
    if phase == 4:
        completion = re.findall(r'H2_STORAGE_ALREADY_COMPLETE no_new_run=(\d+) empty=(\d+) rc=(-?\d+)', current)
        assert completion and all(row == ('1', '1', '0') for row in completion), "completion recheck failed"
        assert 'H2_STORAGE_CASE ' not in current and 'H2_STORAGE_PHASE ' not in current, "completion emitted new cases"
        return dict(phase=phase, nonce=nonce, passed=0, completion_rechecked=True, cases=[])
    cases = [json.loads(row) for row in re.findall(r'H2_STORAGE_CASE (\{[^\r\n]+\})', current)]
    unique = {}
    for case in cases:
        name = case['id']
        assert name in selected, "unexpected case"
        assert case['phase'] == phase and case['nonce'] == nonce, "stale case"
        assert case['status'] == 'PASS' and case['rc'] == 0, "failed case"
        if name in unique:
            assert case == unique[name], "immutable replay changed"
        else:
            unique[name] = case
    reports = [json.loads(row) for row in re.findall(r'H2_STORAGE_PHASE (\{[^\r\n]+\})', current)]
    assert reports, "no terminal phase report"
    for report in reports:
        expected = dict(contract=2, version=version, phase=phase, nonce=nonce,
                        passed=len(selected), failed=0, blocked=0, cleanup=0, rc=0, control=0)
        assert all(report.get(key) == value for key, value in expected.items()), "invalid phase report"
        assert report == reports[0], "terminal replay changed"
    # The initial live output may lose individual UART lines. It cannot be
    # repaired by pooling incomplete replay blocks: one independently framed
    # replay must contain the entire ordered registry and its terminal report.
    headers = list(re.finditer(r'H2_STORAGE_BOOT [^\r\n]*', current))
    boundaries = [0] + [header.end() for header in headers]
    ends = [header.start() for header in headers] + [len(current)]
    complete_replays = 0
    initial_count = 0
    positions = {name: index for index, name in enumerate(selected)}
    for index, (start, end) in enumerate(zip(boundaries, ends)):
        block = current[start:end]
        observed = [json.loads(row)['id'] for row in re.findall(r'H2_STORAGE_CASE (\{[^\r\n]+\})', block)]
        order = [positions[name] for name in observed]
        assert order == sorted(set(order)), "duplicate or reordered ledger block"
        if index == 0:
            initial_count = len(observed)
        elif observed == selected and re.search(r'H2_STORAGE_PHASE (\{[^\r\n]+\})', block):
            complete_replays += 1
    assert complete_replays, "no complete ordered independent replay block"
    return dict(phase=phase, nonce=nonce, passed=len(selected), cases=[unique[name] for name in selected],
                report=reports[0], duplicate_replay_rows=len(cases) - len(selected),
                observed_initial_cases=initial_count, complete_replay_blocks=complete_replays)


def verify_run(texts, registry, version):
    order = (1, 2, 3, 4, 4)
    assert len(texts) == len(order), "five independent boot captures are required"
    phases = [boot_ledger(text, registry, version, phase) for text, phase in zip(texts, order)]
    assert len({phase['nonce'] for phase in phases}) == 1, "cross-boot nonce mismatch"
    assert phases[0]['nonce'] == version_nonce(version), "nonce is not bound to image version"
    cases = [case for phase in phases for case in phase['cases']]
    assert len(cases) == len({case['id'] for case in cases}) == 36
    return dict(contract=2, version=version, nonce=phases[0]['nonce'], passed=36, failed=0, blocked=0,
                ledger_complete=True, phases=phases, cases=cases,
                log_sha256=[hashlib.sha256(text.encode()).hexdigest() for text in texts],
                evidence_boundary='Host reboot receipts establish independent captures; this parser checks boot ledgers. Final UID, image, Loader and coredump identity require separate before/after receipts.')


def fields(text):
    return dict(re.findall(r'(\w+)=([^\s]+)', text))


def final_status(before, after, manifest, package_sha256, uid):
    initial, final = fields(before), fields(after)
    assert initial['device_uid'] == final['device_uid'] == uid, 'device changed'
    assert initial.get('partition_1_valid') == '1' and initial.get('partition_1_role') == 'loader', 'invalid initial Loader'
    loader_keys = [key for key in initial if key.startswith('partition_1_')]
    assert loader_keys, 'missing initial Loader identity'
    assert all(final.get(key) == initial[key] for key in loader_keys), 'Loader changed'
    expected = dict(active_role='app', running_partition='2', next_partition='2', stage_valid='0', partition_2_valid='1',
                    last_result='0', active_version=manifest['version'], active_checksum=manifest['image_sha256'],
                    partition_2_version=manifest['version'], partition_2_image_checksum=manifest['image_sha256'],
                    partition_2_package_checksum=package_sha256)
    assert all(final.get(key) == value for key, value in expected.items()), 'managed image identity mismatch'
    return final


def coredump_status(before, after, before_sha256=None, after_sha256=None):
    initial, final = fields(before), fields(after)
    for key in ('result', 'code', 'bytes', 'stored_bytes', 'blank'):
        assert initial[key] == final[key], 'coredump changed'
    assert final['result'] == 'OK' and final['code'] == '0'
    if final['blank'] != '1':
        assert before_sha256 and before_sha256 == after_sha256, 'nonblank dump requires unchanged actual bytes'
    return final


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--registry', type=Path, default=Path('projects/e2e/apps/pal-storage/app/include/h2_pal_storage_cases.inc'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('logs', type=Path, nargs=5, help='independent boot captures for phases 1, 2, 3, 4, 4')
    args = parser.parse_args()
    report = verify_run([path.read_text() for path in args.logs], args.registry.read_text(), args.version)
    output = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(output)
    else:
        print(output, end='')


if __name__ == '__main__':
    main()
