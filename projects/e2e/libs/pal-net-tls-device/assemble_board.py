"""Bind two real board boots, package bytes, peer run and preserved partitions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'apps/pal-net-tls'))
from check_qualification import check_cases, check_peer
from verify_device import parse

ROOT = Path(__file__).resolve().parents[4]
REGISTRY = re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)',
    (ROOT / 'projects/e2e/apps/pal-net-tls/app/include/h2_pal_net_tls_cases.inc').read_text())


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def status(path, board, uid):
    text = Path(path).read_text(errors='replace')
    matches = re.findall(r'H2_LOADER_STATUS\s+([^\r\n]+)', text)
    assert len(matches) == 1, f'{path}: missing/ambiguous status'
    result = dict(re.findall(r'(\w+)=([^\s]+)', matches[0]))
    assert result['board'] == board and result['device_uid'] == uid
    return result


def dump(path):
    matches = re.findall(r'H2_LOADER_COREDUMP_STATUS\s+([^\r\n]+)',
        Path(path).read_text(errors='replace'))
    assert len(matches) == 1, f'{path}: missing/ambiguous coredump status'
    fields = dict(re.findall(r'(\w+)=([^\s]+)', matches[0]))
    assert fields['result'] == 'OK' and fields['code'] == '0'
    return fields


def assemble(args):
    board_name = 'devkit' if args.board == 'devkit' else 'bk7258_v3_202405'
    version = args.version
    metadata = json.loads(args.firmware_metadata.read_text())
    image = metadata['package_manifest']
    assert image['board'] == board_name and image['version'] == version
    package_sha = digest(args.package)
    assert package_sha == metadata['assets'][0]['sha256']
    image_sha = image['image_sha256']
    base_status = status(args.baseline_status, board_name, args.uid)
    base_dump = dump(args.baseline_dump)
    peer = json.loads(args.peer.read_text())
    assert re.fullmatch('[0-9a-f]{32}', peer['session'])
    boots = []
    for kind in ('install', 'normal-reboot'):
        log = getattr(args, kind.replace('-', '_') + '_log')
        current = status(getattr(args, kind.replace('-', '_') + '_status'), board_name, args.uid)
        dump_status = dump(getattr(args, kind.replace('-', '_') + '_dump'))
        assert current['active_role'] == 'app' and current['active_version'] == version
        assert current['active_checksum'] == image_sha
        assert current['partition_2_image_checksum'] == image_sha
        assert current['partition_1_valid'] == base_status['partition_1_valid'] == '1'
        assert current['partition_1_package_checksum'] == base_status['partition_1_package_checksum']
        assert current['partition_1_image_checksum'] == base_status['partition_1_image_checksum']
        assert current['stage_valid'] == '0' and current['next_partition'] == '2'
        assert current['boot_intent'] == 'auto' and current['last_result'] == '0'
        assert dump_status['blank'] == base_dump['blank']
        assert dump_status['stored_bytes'] == base_dump['stored_bytes']
        entry = parse(log.read_text(errors='replace'), version,
            'devkit' if args.board == 'devkit' else 'bk7258')
        assert entry['session'] == peer['session']
        observation = json.loads(args.dns_observation.read_text())
        assert observation['hostname'] == entry['dns']['host']
        assert observation['ipv4'] == entry['dns']['operator_ipv4']
        entry['dns']['observation'] = observation
        entry.update(kind=kind, uid=args.uid, image_sha256=image_sha,
            package_sha256=package_sha, confirm=0,
            loader_p1_preserved=True, stage_empty=True, coredump_unchanged=True,
            peer=peer,
            observed_status=current, observed_coredump=dump_status,
            observed_boot=dict(board='devkit' if args.board == 'devkit' else 'bk7258',
                version=entry['version'], session=entry['session'], boot_id=entry['boot_id']),
            local_source_sha256=dict(serial=digest(log),
                status=digest(getattr(args, kind.replace('-', '_') + '_status')),
                coredump_status=digest(getattr(args, kind.replace('-', '_') + '_dump'))))
        check_cases(entry, REGISTRY)
        check_peer(peer, entry['session'], entry['boot_id'])
        boots.append(entry)
    assert len({entry['boot_id'] for entry in boots}) == 2, 'stale replay cannot be a normal reboot'
    if args.board == 'bk7258':
        paths = [args.baseline_dump_bytes,args.install_dump_bytes,args.normal_reboot_dump_bytes]
        assert all(path is not None for path in paths)
        baseline = paths[0].read_bytes()
        assert baseline and all(path.read_bytes() == baseline for path in paths[1:])
        coredump_sha = hashlib.sha256(baseline).hexdigest()
        coredump_bytes = dict(zip(('baseline','install','normal-reboot'),
            [path.read_bytes().hex() for path in paths]))
    else:
        assert base_dump['blank'] == '1' and base_dump['stored_bytes'] == '0'
        coredump_sha = None
        coredump_bytes = None
    return dict(observation_contract=2, platform=args.board, status='PASS', core_qualified=True,
        full_net_qualified=False, uid=args.uid, version=version,
        artifact_sha256=package_sha, package_sha256=package_sha,
        image_sha256=image_sha, package_size=args.package.stat().st_size,
        loader_p1_package_sha256=base_status['partition_1_package_checksum'],
        loader_p1_image_sha256=base_status['partition_1_image_checksum'],
        baseline_coredump_sha256=coredump_sha, observed_coredump_bytes=coredump_bytes,
        observed_baseline=dict(status=base_status, coredump=base_dump,
            local_source_sha256=dict(status=digest(args.baseline_status),
                coredump_status=digest(args.baseline_dump))), boots=boots)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', choices=['devkit','bk7258'], required=True)
    parser.add_argument('--uid', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--firmware-metadata', type=Path, required=True)
    parser.add_argument('--peer', type=Path, required=True)
    parser.add_argument('--dns-observation', type=Path, required=True)
    for name in ('baseline-status','baseline-dump','install-log','install-status',
                 'install-dump','normal-reboot-log','normal-reboot-status',
                 'normal-reboot-dump','baseline-dump-bytes','install-dump-bytes',
                 'normal-reboot-dump-bytes'):
        parser.add_argument('--'+name, type=Path, required=name not in (
            'baseline-dump-bytes','install-dump-bytes','normal-reboot-dump-bytes'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(assemble(args), indent=2) + '\n')
    print('PAL Net/TLS board:', args.board, '2/2 real boots and peer ledger PASS')
