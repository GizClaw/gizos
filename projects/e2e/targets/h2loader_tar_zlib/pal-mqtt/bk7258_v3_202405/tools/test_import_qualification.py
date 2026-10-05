"""Format/admission guard tests only; these do not create E2E qualification."""
import json
import runpy
from pathlib import Path
import tempfile
import unittest

from import_qualification import admitted, captured_verifier, import_run, sha, write


class AdmissionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        self.source = 'a' * 40
        self.version = 'mqtt-bk-guard-test'
        (self.folder / 'strict-verifier.log').write_text('format guard test only\n')
        self.collector = dict(qualified=True, strict_verifier_exit=0,
            package_sha256='b' * 64,
            logs_sha256={'strict-verifier.log': sha(self.folder / 'strict-verifier.log')})
        write(self.folder / 'collector-receipt.json', self.collector)
        write(self.folder / 'source-boundary.json', dict(source_commit=self.source, source_dirty=False))
        write(self.folder / 'build-receipt.json', dict(actual_exit=0, source_commit=self.source,
            argv=['--//tools/bazel:firmware_version=' + self.version]))
        write(self.folder / 'package-binding.json', dict(source_commit=self.source, source_dirty=False,
            manifest=dict(version=self.version), package_sha256='b' * 64))

    def update(self, name, **values):
        path = self.folder / name
        original = json.loads(path.read_text())
        original.update(values)
        path.write_text(json.dumps(original))

    def test_format_guard_admits_exact_true_and_integer_exit_zero(self):
        self.assertEqual(admitted(self.folder, self.source, self.version), self.collector)

    def test_pending_and_failed_collector_rejected(self):
        for value in (False, None, 1, 'true'):
            with self.subTest(value=value):
                self.update('collector-receipt.json', qualified=value)
                with self.assertRaises(AssertionError):admitted(self.folder, self.source, self.version)

    def test_failed_or_boolean_strict_exit_rejected(self):
        for value in (1, False, None, '0'):
            with self.subTest(value=value):
                self.update('collector-receipt.json', strict_verifier_exit=value)
                with self.assertRaises(AssertionError):admitted(self.folder, self.source, self.version)

    def test_changed_raw_log_rejected(self):
        (self.folder / 'strict-verifier.log').write_text('changed after capture\n')
        with self.assertRaises(AssertionError):admitted(self.folder, self.source, self.version)

    def test_other_source_or_dirty_boundary_rejected(self):
        with self.assertRaises(AssertionError):admitted(self.folder, 'c' * 40, self.version)
        self.update('source-boundary.json', source_dirty=True)
        with self.assertRaises(AssertionError):admitted(self.folder, self.source, self.version)

    def test_other_package_or_version_rejected(self):
        with self.assertRaises(AssertionError):admitted(self.folder, self.source, self.version + '-other')
        self.update('collector-receipt.json', package_sha256='d' * 64)
        with self.assertRaises(AssertionError):admitted(self.folder, self.source, self.version)

    def test_reused_package_requires_actual_original_build_receipt(self):
        original = self.folder / 'original-build.json'
        (self.folder / 'build-receipt.json').rename(original)
        with self.assertRaises(FileNotFoundError):admitted(self.folder, self.source, self.version)
        self.assertEqual(admitted(self.folder, self.source, self.version, original), self.collector)
        self.update('original-build.json', source_commit='c' * 40)
        with self.assertRaises(AssertionError):admitted(self.folder, self.source, self.version, original)

    def test_previous_destination_never_overwritten(self):
        destination = self.folder / 'old'
        destination.mkdir()
        sentinel = destination / 'old-qualified.json'
        sentinel.write_text('original immutable record')
        with self.assertRaises(AssertionError):
            import_run(self.folder, destination, Path('/unused'), self.source, self.version, 'unused', 'e' * 64)
        self.assertEqual(sentinel.read_text(), 'original immutable record')

    def test_historical_verifier_is_selected_and_bound_to_the_capture(self):
        verifier=self.folder/'host-verifier.py'
        verifier.write_text('captured verifier bytes\n')
        collector={'verifier_sha256':sha(verifier)}
        self.assertEqual(captured_verifier(self.folder,collector),verifier)
        # Current checkout revisions do not replace the capture's authority.
        current=self.folder/'current-verifier.py';current.write_text('new stricter verifier\n')
        self.assertNotEqual(sha(current),collector['verifier_sha256'])
        verifier.write_text('modified historical verifier\n')
        with self.assertRaisesRegex(AssertionError,'differs'):captured_verifier(self.folder,collector)
        verifier.unlink()
        with self.assertRaisesRegex(AssertionError,'missing captured'):captured_verifier(self.folder,collector)

    def test_original_captured_verifier_matches_the_immutable_collector(self):
        folder=Path(__file__).parent.parent/'evidence/runs/20a23ce1-r12-continuous'
        collector=json.loads((folder/'collector-receipt.json').read_text())
        self.assertEqual(captured_verifier(folder,collector),folder/'host-verifier.py')

    def test_captured_verifier_preserves_its_original_confirmation_schema(self):
        folder=Path(__file__).parent.parent/'evidence/runs/20a23ce1-r12-continuous'
        collector=json.loads((folder/'collector-receipt.json').read_text())
        original=runpy.run_path(str(captured_verifier(folder,collector)),run_name='historical_contract')
        ids=['case-'+str(index) for index in range(36)]
        identity='a'*32+'-'+'b'*16
        boot='id='+identity+' version=v1 epoch_ms=123 ca_sha256='+'c'*64
        rows='\n'.join('H2_PAL_MQTT_CASE '+json.dumps({'id':case,'status':'PASS','detail':0}) for case in ids)
        summary=dict(selected=36,passed=36,failed=0,blocked=0,cleanup=0,rc=0,before=[0]*10,after=[0]*10)
        text='H2_PAL_MQTT_PLATFORM_BOOT board=bk7258\nH2_PAL_MQTT_BOOT '+boot+'\nH2_PAL_MQTT_RUN '+boot+'\n'+rows+ \
             '\nH2_PAL_MQTT_SUMMARY '+json.dumps(summary)+'\nH2_PAL_MQTT_READY board=bk7258 rc=0 confirm=0\n'
        self.assertEqual(original['boot_ledger'](text,ids,'v1')['ready']['confirm'],'0')


if __name__ == '__main__':
    unittest.main()
