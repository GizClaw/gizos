"""Negative checks that stale or partial UART output cannot qualify a board."""
import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).parent))
from verify_device import boot_ledger

class LedgerTest(unittest.TestCase):
    registry = 'H2_PAL_ADEC_CASE("pcm", test_pcm)\n'
    case = dict(version='new', id='pcm', status='PASS', detail=0, line=0)
    report = dict(version='new', contract=1, operations=8, passed=1, failed=0,
                  blocked=0, retained=0, qualified=1, rc=0, frames=1, pcm_bytes=2048)

    def ledger(self, marker=True, cases=None, ready=True):
        return ('H2_ADEC_BOOT version=new\n' if marker else '') + ''.join(
            'H2_ADEC_CASE '+json.dumps(c)+'\n' for c in (cases if cases is not None else [self.case])) + \
            'H2_ADEC_REPORT '+json.dumps(self.report)+'\n'+('H2_ADEC_READY rc=0 confirm=0\n' if ready else '')

    def test_new_run_with_old_replay(self):
        boot_ledger(self.ledger(marker=False)+self.ledger(), self.registry, 'new')

    def test_replay_only(self):
        with self.assertRaises(AssertionError): boot_ledger(self.ledger(marker=False), self.registry, 'new')

    def test_partial_duplicate_or_failed_cases(self):
        for cases in ([], [self.case, self.case], [dict(self.case, detail=-7)]):
            with self.assertRaises(AssertionError): boot_ledger(self.ledger(cases=cases), self.registry, 'new')

    def test_unconfirmed(self):
        with self.assertRaises(AssertionError): boot_ledger(self.ledger(ready=False), self.registry, 'new')

if __name__ == '__main__': unittest.main()
