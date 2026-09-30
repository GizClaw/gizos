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
        return ('H2_ATOMIC_EXECUTION '+json.dumps(dict(version='r1',execution='a'*32,cleanup=0,confirm=0))+'\n' + ''.join('H2_ATOMIC_CASE '+json.dumps(r)+'\n' for r in rows)
                + 'H2_ATOMIC_REPORT '+json.dumps(report)+'\nH2_ATOMIC_CLEANUP rc=0\nH2_ATOMIC_READY rc=0 confirm=0\n')
    def test_complete_fresh_boot(self):
        self.assertEqual(boot_ledger(self.valid(),self.ids,'r1')['passed'],56)
    def test_stale_replay_cannot_qualify(self):
        with self.assertRaises(AssertionError):boot_ledger(self.valid(),self.ids,'r1','a'*32)
    def test_new_boot_excludes_old_replay(self):
        text=self.valid()+self.valid().replace('a'*32,'b'*32)
        self.assertEqual(boot_ledger(text,self.ids,'r1','a'*32)['execution'],'b'*32)
    def test_partial_replay_waits_for_complete_cycle(self):
        first=self.valid().split('H2_ATOMIC_REPORT')[0]
        self.assertEqual(boot_ledger(first+self.valid(),self.ids,'r1')['passed'],56)
    def test_case_rows_cannot_cross_execution_identity(self):
        first=self.valid().split('H2_ATOMIC_REPORT')[0]
        final=self.valid().replace('a'*32,'b'*32)
        final='\n'.join(line for line in final.splitlines()
                        if 'H2_ATOMIC_CASE' not in line)
        with self.assertRaises(AssertionError):boot_ledger(first+final,self.ids,'r1')
    def test_failed_case_is_rejected(self):
        with self.assertRaises(AssertionError):boot_ledger(self.valid().replace('"status": "PASS"','"status": "FAIL"',1),self.ids,'r1')
    def test_unjoined_worker_is_rejected(self):
        with self.assertRaises(AssertionError):boot_ledger(self.valid().replace('"workers_joined": 20','"workers_joined": 19'),self.ids,'r1')
    def test_cleanup_and_confirmation_required(self):
        for replacement in [('"cleanup": 0','"cleanup": -7'),('"confirm": 0','"confirm": -7')]:
            with self.assertRaises(AssertionError):boot_ledger(self.valid().replace(*replacement),self.ids,'r1')
if __name__=='__main__':unittest.main()
