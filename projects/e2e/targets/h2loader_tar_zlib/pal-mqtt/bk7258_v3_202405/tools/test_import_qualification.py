"""Format/admission guard tests only; these do not create E2E qualification."""
import json
from pathlib import Path
import tempfile
import unittest

from import_qualification import admitted, import_run, sha, write


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

    def test_previous_destination_never_overwritten(self):
        destination = self.folder / 'old'
        destination.mkdir()
        sentinel = destination / 'old-qualified.json'
        sentinel.write_text('original immutable record')
        with self.assertRaises(AssertionError):
            import_run(self.folder, destination, Path('/unused'), self.source, self.version, 'unused', 'e' * 64)
        self.assertEqual(sentinel.read_text(), 'original immutable record')


if __name__ == '__main__':
    unittest.main()
