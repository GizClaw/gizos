import json
import unittest
from run_device import boot_ledger

class BootLedgerTest(unittest.TestCase):
    ids = [f'case{i}' for i in range(28)]
    def valid(self):
        rows = [dict(version='r1',placement=p,id=i,status='PASS',rc=0)
                for p in ['internal','psram-wrapper'] for i in self.ids]
        report = dict(version='r1',passed=56,failed=0,not_run=0,workers_started=20,
                      workers_joined=20,qualified=1,teardown=0)
        return ('H2_ATOMIC_BOOT version=r1\n' + ''.join('H2_ATOMIC_CASE '+json.dumps(r)+'\n' for r in rows)
                + 'H2_ATOMIC_REPORT '+json.dumps(report)+'\nH2_ATOMIC_CLEANUP rc=0\nH2_ATOMIC_READY rc=0 confirm=0\n')
    def test_complete_fresh_boot(self):
        self.assertEqual(boot_ledger(self.valid(),self.ids,'r1')['passed'],56)
    def test_stale_replay_cannot_qualify(self):
        text=self.valid().replace('H2_ATOMIC_BOOT version=r1\n','')
        with self.assertRaises(AssertionError):boot_ledger(text,self.ids,'r1')
    def test_repeated_boot_is_rejected(self):
        with self.assertRaises(AssertionError):boot_ledger(self.valid()+'H2_ATOMIC_BOOT version=r1\n',self.ids,'r1')
    def test_failed_case_is_rejected(self):
        with self.assertRaises(AssertionError):boot_ledger(self.valid().replace('"status": "PASS"','"status": "FAIL"',1),self.ids,'r1')
    def test_unjoined_worker_is_rejected(self):
        with self.assertRaises(AssertionError):boot_ledger(self.valid().replace('"workers_joined": 20','"workers_joined": 19'),self.ids,'r1')
    def test_cleanup_and_confirmation_required(self):
        for marker in ['H2_ATOMIC_CLEANUP rc=0','H2_ATOMIC_READY rc=0 confirm=0']:
            with self.assertRaises(AssertionError):boot_ledger(self.valid().replace(marker,''),self.ids,'r1')
if __name__=='__main__':unittest.main()
