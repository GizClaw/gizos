"""Negative admission checks only: never manufacture a qualified board receipt."""
from pathlib import Path
import importlib.util
import json
import tempfile
import unittest

MODULE_PATH = Path(__file__).with_name('record_bk_r4_qualification.py')
spec = importlib.util.spec_from_file_location('record_bk_r4_gate', MODULE_PATH)
record = importlib.util.module_from_spec(spec)
spec.loader.exec_module(record)


class QualificationAdmission(unittest.TestCase):
    def test_missing_or_unqualified_never_admitted(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            with self.assertRaises(FileNotFoundError):
                record.validate(directory)
            for qualified in (False, 'true', 1, None):
                (directory / 'qualified.json').write_text(json.dumps({'qualified': qualified}))
                with self.assertRaises(AssertionError):
                    record.validate(directory)

    def test_source_and_result_cannot_be_rebound(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            base = dict(qualified=True, contract=2, version=record.VERSION,
                        source_revision=record.SOURCE, package_sha256=record.PACKAGE,
                        image_sha256=record.IMAGE, uid=record.UID, port=record.PORT,
                        passed=36, failed=0, blocked=0, ledger_complete=True)
            for key, bad in [('source_revision', 'de68' * 10), ('version', 'old-version'),
                             ('passed', 31), ('failed', 1), ('ledger_complete', False),
                             ('package_sha256', '0' * 64), ('image_sha256', '0' * 64)]:
                row = dict(base, **{key: bad})
                (directory / 'qualified.json').write_text(json.dumps(row))
                with self.assertRaises(AssertionError):
                    record.validate(directory)

    def test_interrupt_admission_requires_controlled_full_command(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            log = directory / 'capture.log'; log.write_text('native marker\n')
            path = directory / 'receipt.json'
            base = dict(command=['reboot', 'app', '--monitor'], port=record.PORT,
                        actual_exit=130, controlled_stop=True, stop_signal='SIGINT',
                        log_sha256=record.sha(log), started_at_utc='2026-10-04T12:00:00+00:00',
                        finished_at_utc='2026-10-04T12:01:00+00:00')
            for change in ({'controlled_stop': False}, {'stop_signal': 'SIGTERM'}, {'actual_exit': 3}):
                path.write_text(json.dumps(dict(base, **change)))
                with self.assertRaises(AssertionError):
                    record.command_receipt(path, log, base['command'], monitor=True)
            path.write_text(json.dumps(base))
            with self.assertRaises(AssertionError):
                record.command_receipt(path, log, base['command'], monitor=False)
            self.assertEqual(record.command_receipt(path, log, base['command'], monitor=True)['actual_exit'], 130)


if __name__ == '__main__':
    unittest.main()
