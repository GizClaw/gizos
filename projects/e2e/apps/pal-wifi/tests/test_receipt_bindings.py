"""Fault tests of receipt bindings; these do not qualify a real WLAN provider."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sys
import tarfile
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path('projects/e2e/apps/pal-wifi').resolve()))
sys.path.insert(0, str(Path.cwd()))
from receipt_bindings import check_status, capture_snapshot, check_captured_status
from projects.h2loader.tools.bazel.firmware_artifacts import package_manifest, write_package


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class ReceiptBindingFailures(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.package = self.root / 'package.update.tar.zlib'
        self.image = bytes(range(256)) * 2
        self.identity = dict(role='app', board='bk7258_v3_202405', target='bk7258', version='unit-v1')
        self.app_path = 'app/bk/app_ab_crc.rbl'
        write_package(self.package, self.app_path, self.image, [], **self.identity)
        manifest = package_manifest(self.image, **self.identity)
        self.metadata = dict(self.identity, assets=[dict(sha256=digest(self.package), size=self.package.stat().st_size)], package_manifest=manifest)
        self.meta_path = self.root / 'firmware.json'
        self.meta_path.write_text(json.dumps(self.metadata))
        p1 = dict(partition_1_valid='1', partition_1_role='loader', partition_1_version='baseline',
                  partition_1_board=self.identity['board'], partition_1_target='bk7258',
                  partition_1_image_checksum='1' * 64, partition_1_package_checksum='2' * 64,
                  partition_1_image_size='1024', partition_1_package_size='512')
        self.baseline = dict(device_uid='test-uid', **p1)
        self.stage = dict(device_uid='test-uid', stage_valid='1')
        final = dict(device_uid='test-uid', stage_valid='0', running_partition='2', next_partition='2',
                     last_result='0', active_role='app', active_version='unit-v1',
                     active_checksum=manifest['image_sha256'], active_image_size=str(len(self.image)), **p1)
        for field, value in dict(role='app', board=self.identity['board'], target='bk7258', version='unit-v1',
                                 image_checksum=manifest['image_sha256'], image_size=str(len(self.image)),
                                 package_checksum=digest(self.package), package_size=str(self.package.stat().st_size)).items():
            self.stage['stage_' + field] = value
            final['partition_2_' + field] = value
        self.dump = dict(result='OK', code='0', partition='coredump', bytes='4096', stored_bytes='32', blank='0')
        self.source = self.root / 'source.c'
        self.source.write_text('source fixture\n')
        self.report = dict(version='unit-v1', uid='test-uid',
                           package_sha256=digest(self.package), image_sha256=manifest['image_sha256'],
                           firmware=self.ref(self.meta_path), package=self.ref(self.package),
                           baseline_status=self.status('baseline.log', self.baseline),
                           stage_status=self.status('stage.log', self.stage),
                           baseline_coredump=self.status('old-dump.log', self.dump, coredump=True),
                           baseline_coredump_bytes=self.file('old-dump.bin', b'X' * 32),
                           source_inputs={str(self.source):digest(self.source)}, runs=[dict(version='seed')])
        for n in range(2):
            self.report['runs'].append(dict(final_status=self.status(f'final-{n}.log', final),
                final_coredump=self.status(f'dump-{n}.log', self.dump, coredump=True),
                final_coredump_bytes=self.file(f'dump-{n}.bin', b'X' * 32)))

    def ref(self, path):
        return dict(path=str(path), sha256=digest(path))

    def file(self, name, contents):
        path = self.root / name
        path.write_bytes(contents)
        return self.ref(path)

    def status(self, name, values, coredump=False):
        marker = 'H2_LOADER_COREDUMP_STATUS ' if coredump else 'H2_LOADER_STATUS '
        return self.file(name, (marker + ' '.join(f'{k}={v}' for k,v in values.items()) + '\n').encode())

    def alter(self, ref, key, value):
        path = Path(ref['path'])
        text, n = re.subn(r'(?<!\S)' + re.escape(key) + r'=\S+', key+'='+value, path.read_text())
        self.assertEqual(n, 1)
        path.write_text(text)
        ref['sha256'] = digest(path)

    def replace_member(self, member, data):
        stream = io.BytesIO()
        with tarfile.open(fileobj=io.BytesIO(zlib.decompress(self.package.read_bytes()))) as old:
            entries = [(m.name, old.extractfile(m).read()) for m in old.getmembers()]
        with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as new:
            for name, payload in entries:
                if name == member:
                    payload = data(payload)
                info = tarfile.TarInfo(name)
                info.size = len(payload)
                new.addfile(info, io.BytesIO(payload))
        self.package.write_bytes(zlib.compress(stream.getvalue()))
        sha, size = digest(self.package), self.package.stat().st_size
        self.report['package'] = self.ref(self.package)
        self.report['package_sha256'] = sha
        self.metadata['assets'][0].update(sha256=sha, size=size)
        self.meta_path.write_text(json.dumps(self.metadata))
        self.report['firmware'] = self.ref(self.meta_path)
        self.alter(self.report['stage_status'], 'stage_package_checksum', sha)
        self.alter(self.report['stage_status'], 'stage_package_size', str(size))
        for run in self.report['runs'][1:]:
            self.alter(run['final_status'], 'partition_2_package_checksum', sha)
            self.alter(run['final_status'], 'partition_2_package_size', str(size))

    def test_valid_actual_archive_and_two_statuses(self):
        check_status(self.report)

    def test_package_substitution(self):
        self.package.write_bytes(self.package.read_bytes()+b'substitution')
        with self.assertRaises(AssertionError):
            check_status(self.report)

    def test_embedded_manifest_disagrees_despite_matching_outer_package_hash(self):
        self.replace_member('manifest', lambda x:x.replace(b'version=unit-v1', b'version=unit-v2'))
        with self.assertRaises(AssertionError):
            check_status(self.report)

    def test_corrupt_raw_image_despite_matching_outer_package_hash(self):
        self.replace_member(self.app_path, lambda x:b'Y'+x[1:])
        with self.assertRaises(AssertionError):
            check_status(self.report)

    def test_final_uid_version_image_p1_and_stage_rejected(self):
        for key,value in [('device_uid','wrong-uid'),('active_version','wrong-version'),
                          ('active_checksum','0'*64),('partition_1_image_checksum','0'*64),
                          ('stage_valid','1')]:
            with self.subTest(field=key):
                ref=self.report['runs'][1]['final_status']
                original=Path(ref['path']).read_bytes()
                self.alter(ref,key,value)
                with self.assertRaises(AssertionError):check_status(self.report)
                Path(ref['path']).write_bytes(original)
                ref['sha256']=digest(ref['path'])

    def test_changed_coredump_bytes_rejected(self):
        self.report['runs'][1]['final_coredump_bytes']=self.file('changed-dump.bin',b'Y'*32)
        with self.assertRaises(AssertionError):check_status(self.report)

    def test_missing_p1_baseline_rejected(self):
        self.report['baseline_status']=self.status('empty-baseline.log',dict(device_uid='test-uid'))
        with self.assertRaises(AssertionError):check_status(self.report)

    def test_truncated_identical_dumps_do_not_match_stored_byte_count(self):
        self.report['baseline_coredump_bytes']=self.file('short-old.bin',b'X')
        for run in self.report['runs'][1:]:
            run['final_coredump_bytes']=self.file('short-final.bin',b'X')
        with self.assertRaises(AssertionError):check_status(self.report)

    def test_source_drift_rejected(self):
        self.source.write_text('changed source\n')
        with self.assertRaises(AssertionError):check_status(self.report)

    def captured(self):
        cwd = Path.cwd()
        self.addCleanup(os.chdir, cwd)
        os.chdir(self.root)
        self.report['bindings_snapshot'] = capture_snapshot(self.report, 'captured.json')
        return Path('captured.json')

    def test_portable_capture_does_not_read_unavailable_local_artifacts(self):
        self.captured()
        for field in ('firmware', 'package', 'baseline_status', 'stage_status',
                      'baseline_coredump', 'baseline_coredump_bytes'):
            Path(self.report[field]['path']).unlink()
        for run in self.report['runs'][1:]:
            for field in ('final_status', 'final_coredump', 'final_coredump_bytes'):
                Path(run[field]['path']).unlink()
        check_captured_status(self.report)
        with self.assertRaises(FileNotFoundError):
            check_status(self.report)

    def test_portable_capture_rejects_tampered_identity_and_missing_boots(self):
        path = self.captured()
        original = json.loads(path.read_text())
        for change in ('uid', 'p1', 'stage', 'dump', 'boots', 'method', 'raw_hashes'):
            with self.subTest(change=change):
                value = copy.deepcopy(original)
                if change == 'uid': value['uid'] = 'different-device'
                elif change == 'p1': value['runs'][0]['final_status']['partition_1_image_checksum'] = '0' * 64
                elif change == 'stage': value['runs'][0]['final_status']['stage_valid'] = '1'
                elif change == 'dump': value['runs'][0]['final_coredump_bytes']['size'] = 1
                elif change == 'boots': value['runs'].pop()
                elif change == 'method': value['capture_method'] = 'built-only'
                else: value['raw_hashes'].clear()
                path.write_text(json.dumps(value))
                self.report['bindings_snapshot']['sha256'] = digest(path)
                with self.assertRaises(AssertionError):
                    check_captured_status(self.report)

    def test_live_audit_rejects_disagreement_with_captured_record(self):
        path = self.captured()
        value = json.loads(path.read_text())
        value['stage_status']['stage_version'] = 'another-artifact'
        path.write_text(json.dumps(value))
        self.report['bindings_snapshot']['sha256'] = digest(path)
        with self.assertRaises(AssertionError):
            check_status(self.report)


if __name__=='__main__':
    unittest.main()
