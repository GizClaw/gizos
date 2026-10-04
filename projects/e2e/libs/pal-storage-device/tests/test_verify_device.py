"""Reject stale, partial and mutated Storage boot evidence."""
import json
from pathlib import Path
import re
import unittest

from verify_device import boot_ledger, coredump_status, final_status, verify_run, version_nonce


REGISTRY = Path('projects/e2e/apps/pal-storage/app/include/h2_pal_storage_cases.inc').read_text()
VERSION = 'pref-ledger-test'
NONCE = version_nonce(VERSION)


def capture(phase):
    marker = f'H2_STORAGE_BOOT contract=2 version={VERSION} phase={phase} nonce={NONCE}'
    if phase == 4:
        return '\n'.join((marker, 'H2_STORAGE_ALREADY_COMPLETE no_new_run=1 empty=1 rc=0',
                          'H2_STORAGE_READY rc=0 confirm=0', marker + ' replay=1',
                          'H2_STORAGE_ALREADY_COMPLETE no_new_run=1 empty=1 rc=0'))
    names = [name for name, selected in re.findall(r'H2_PAL_STORAGE_CASE\("([^\"]+)", ([123])\)', REGISTRY) if int(selected) == phase]
    rows = ['H2_STORAGE_CASE ' + json.dumps(dict(id=name, phase=phase, nonce=NONCE, status='PASS', rc=0)) for name in names]
    report = dict(contract=2, version=VERSION, phase=phase, nonce=NONCE, passed=len(names), failed=0,
                  blocked=0, cleanup=0, rc=0, control=0)
    return '\n'.join([marker, *rows, 'H2_STORAGE_READY rc=0 confirm=0', marker + ' replay=1',
                      *rows, 'H2_STORAGE_PHASE ' + json.dumps(report)])


class LedgerTest(unittest.TestCase):
    def test_complete_run_uses_unique_cases_and_immutable_replays(self):
        report = verify_run([capture(phase) for phase in (1, 2, 3, 4, 4)], REGISTRY, VERSION)
        self.assertTrue(report['ledger_complete'])
        self.assertEqual(report['passed'], 36)
        self.assertEqual([phase['passed'] for phase in report['phases']], [31, 3, 2, 0, 0])
        self.assertEqual(report['phases'][0]['duplicate_replay_rows'], 31)

    def test_old_pre_reboot_replay_does_not_count_as_new_boot(self):
        stale = capture(1).replace(f'nonce={NONCE}', 'nonce=12').replace('phase=1 ', 'phase=1 replay=1 ')
        report = boot_ledger(stale + '\n' + capture(2), REGISTRY, VERSION, 2)
        self.assertEqual(report['passed'], 3)
        with self.assertRaises(AssertionError):
            boot_ledger(capture(2).replace(' nonce=', ' replay=1 nonce='), REGISTRY, VERSION, 2)

    def test_mutated_replay_failed_result_and_watchdog_are_rejected(self):
        text = capture(2)
        for altered in (text.replace('"rc": 0', '"rc": -4', 1),
                        text.replace('"nonce": ' + str(NONCE), '"nonce": 12', 1),
                        text.replace('confirm=0', 'confirm=1'),
                        text.replace('"control": 0', '"control": -4'),
                        text + '\nH2_STORAGE_WATCHDOG timeout=600s',
                        text + '\nH2_STORAGE_CASE ' + json.dumps(dict(id='unknown', phase=2, nonce=NONCE, status='PASS', rc=0))):
            with self.subTest(text=altered):
                with self.assertRaises(AssertionError):
                    boot_ledger(altered, REGISTRY, VERSION, 2)

    def test_same_nonce_replay_rows_and_terminal_reports_are_immutable(self):
        lines = capture(2).splitlines()
        cases = [index for index, line in enumerate(lines) if line.startswith('H2_STORAGE_CASE ')]
        changed = lines.copy()
        row = json.loads(changed[cases[-1]].removeprefix('H2_STORAGE_CASE '))
        row['extra'] = 'changed replay'
        changed[cases[-1]] = 'H2_STORAGE_CASE ' + json.dumps(row)
        with self.assertRaises(AssertionError):
            boot_ledger('\n'.join(changed), REGISTRY, VERSION, 2)
        report = json.loads(lines[-1].removeprefix('H2_STORAGE_PHASE '))
        report['extra'] = 'changed terminal replay'
        with self.assertRaises(AssertionError):
            boot_ledger(capture(2) + '\nH2_STORAGE_PHASE ' + json.dumps(report), REGISTRY, VERSION, 2)

    def test_partial_or_reordered_first_ledger_is_rejected(self):
        lines = capture(2).splitlines()
        for changed in (lines[:1] + lines[2:], [lines[0], lines[2], lines[1], *lines[3:]]):
            with self.assertRaises(AssertionError):
                boot_ledger('\n'.join(changed), REGISTRY, VERSION, 2)

    def test_completion_and_all_five_boots_are_required(self):
        for changed in (capture(4).replace('empty=1', 'empty=0'),
                        capture(4) + '\nH2_STORAGE_CASE {}', capture(4).replace('rc=0', 'rc=-4', 1)):
            with self.assertRaises(AssertionError):
                boot_ledger(changed, REGISTRY, VERSION, 4)
        with self.assertRaises(AssertionError):
            verify_run([capture(phase) for phase in (1, 2, 3, 4)], REGISTRY, VERSION)
        with self.assertRaises(AssertionError):
            boot_ledger(capture(2) + '\n' + capture(2), REGISTRY, VERSION, 2)

    def test_later_boot_without_new_cases_cannot_reuse_old_complete_ledger(self):
        for header in (
            f'H2_STORAGE_BOOT contract=2 version={VERSION} phase=3 nonce={NONCE}',
            f'H2_STORAGE_BOOT contract=2 version=another-image phase=2 nonce={NONCE}',
            f'H2_STORAGE_BOOT contract=1 version={VERSION} phase=2 nonce={NONCE}',
            f'H2_STORAGE_BOOT contract=2 version={VERSION} phase=2 nonce=12 replay=1',
            f'H2_STORAGE_BOOT contract=2 version=another-image phase=2 nonce={NONCE} replay=1',
        ):
            with self.subTest(header=header):
                with self.assertRaises(AssertionError):
                    boot_ledger(capture(2) + '\n' + header, REGISTRY, VERSION, 2)
                with self.assertRaises(AssertionError):
                    boot_ledger(capture(4) + '\n' + header, REGISTRY, VERSION, 4)

    def test_final_identity_and_coredump_must_match(self):
        before = 'device_uid=uid partition_1_valid=1 partition_1_role=loader partition_1_version=loader partition_1_image_checksum=p1'
        manifest = dict(version=VERSION, image_sha256='image')
        after = before + f' active_role=app running_partition=2 next_partition=2 stage_valid=0 last_result=0 active_version={VERSION} active_checksum=image partition_2_valid=1 partition_2_version={VERSION} partition_2_image_checksum=image partition_2_package_checksum=package'
        final_status(before, after, manifest, 'package', 'uid')
        for changed in (after.replace('stage_valid=0', 'stage_valid=1'), after.replace('device_uid=uid', 'device_uid=other'), after.replace('checksum=p1', 'checksum=changed'), after.replace('partition_2_valid=1', 'partition_2_valid=0')):
            with self.assertRaises(AssertionError):
                final_status(before, changed, manifest, 'package', 'uid')
        for changed in (before.replace('partition_1_valid=1', 'partition_1_valid=0'), before.replace('partition_1_role=loader', 'partition_1_role=app')):
            with self.assertRaises(AssertionError):
                final_status(changed, after.replace(before, changed), manifest, 'package', 'uid')
        blank = 'result=OK code=0 bytes=65536 stored_bytes=0 blank=1'
        coredump_status(blank, blank)
        with self.assertRaises(AssertionError):
            coredump_status(blank, blank.replace('stored_bytes=0', 'stored_bytes=4'))
        nonblank = blank.replace('stored_bytes=0', 'stored_bytes=4').replace('blank=1', 'blank=0')
        with self.assertRaises(AssertionError):
            coredump_status(nonblank, nonblank)
        coredump_status(nonblank, nonblank, 'actual-hash', 'actual-hash')


if __name__ == '__main__':
    unittest.main()
