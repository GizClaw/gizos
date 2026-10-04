import json
import unittest
import hashlib
import io
import tarfile
import zlib
from run_device import after_reboot, boot_ledger, package_identity

class PackageIdentityTest(unittest.TestCase):
    def package(self, version):
        image=b'atomic-image'
        compressed=zlib.compress(image)
        manifest=(f'format={version}\nrole=app\nboard=fixture\ntarget=esp32s3\nversion=r1\n'
                  f'image_size={len(image)}\nimage_sha256={hashlib.sha256(image).hexdigest()}\n')
        if version==2:
            manifest+=f'app_zlib_size={len(compressed)}\napp_zlib_sha256={hashlib.sha256(compressed).hexdigest()}\n'
        entries=[('manifest',manifest.encode())]
        entries += [('data.tar.zlib',zlib.compress(bytes(1024))),('app.bin.zlib',compressed)] if version==2 else [('app/esp/app.bin',image)]
        out=io.BytesIO()
        with tarfile.open(fileobj=out,mode='w',format=tarfile.USTAR_FORMAT) as tar:
            for name,data in entries:
                member=tarfile.TarInfo(name);member.size=len(data)
                tar.addfile(member,io.BytesIO(data))
        return out.getvalue() if version==2 else zlib.compress(out.getvalue())
    def test_new_package(self):
        self.assertEqual(package_identity(self.package(2),'esp32s3')['format'],'2')
    def test_historical_package(self):
        self.assertEqual(package_identity(self.package(1),'esp32s3')['format'],'1')
    def test_wrong_target(self):
        with self.assertRaises(AssertionError):package_identity(self.package(2),'bk7258')
    def test_wrong_compressed_identity(self):
        original=self.package(2)
        old=hashlib.sha256(zlib.compress(b'atomic-image')).hexdigest().encode()
        corrupted=original.replace(old,b'0'*64)
        with self.assertRaises(AssertionError):package_identity(corrupted,'esp32s3')

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
    def test_replay_before_accepted_reboot_is_excluded(self):
        with self.assertRaises(AssertionError):after_reboot(self.valid(),'upgrade')
        text=self.valid()+'H2_LOADER_REBOOT target=upgrade result=accepted\n'
        with self.assertRaises(AssertionError):boot_ledger(after_reboot(text,'upgrade'),self.ids,'r1')
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
