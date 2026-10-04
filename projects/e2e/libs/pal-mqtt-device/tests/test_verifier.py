import json
from pathlib import Path
import re
import unittest
from verify_device import boot_ledger,status_preserved,coredump_preserved
class Verifier(unittest.TestCase):
    def setUp(self):
        root=Path(__file__).absolute().parents[5]
        registry=root/'projects/e2e/apps/pal-mqtt/app/include/h2_pal_mqtt_cases.inc'
        self.ids=re.findall(r'H2_PAL_MQTT_CASE\(\w+, "([^"]+)"\)',registry.read_text())
        self.execution='a'*32+'-'+'b'*16
        self.boot=f'id={self.execution} version=v1 epoch_ms=123 ca_sha256='+('c'*64)
        rows='\n'.join('H2_PAL_MQTT_CASE '+json.dumps(dict(id=case,status='PASS',detail=0)) for case in self.ids)
        summary=dict(selected=36,passed=36,failed=0,blocked=0,cleanup=0,rc=0,before=[0]*10,after=[0]*10)
        self.good='H2_PAL_MQTT_PLATFORM_BOOT board=bk7258\nH2_PAL_MQTT_BOOT '+self.boot+'\nH2_PAL_MQTT_RUN '+self.boot+'\n'+rows+'\nH2_PAL_MQTT_SUMMARY '+json.dumps(summary)+'\nH2_PAL_MQTT_READY board=bk7258 rc=0 confirm=0\n'
    def test_complete(self):
        self.assertEqual(boot_ledger(self.good,self.ids,'v1')['boot']['id'],self.execution)
    def test_real_log_prefix_fields_and_color(self):
        prefixed='\n'.join('\x1b[32mcpu=0 tick='+str(index)+' '+line+'\x1b[0m'
            for index,line in enumerate(self.good.splitlines()))
        self.assertEqual(boot_ledger(prefixed,self.ids,'v1')['boot']['id'],self.execution)
    def test_late_platform_boot(self):
        with self.assertRaises(AssertionError):boot_ledger(self.good+'H2_PAL_MQTT_PLATFORM_BOOT board=bk7258\n',self.ids,'v1')
    def test_late_other_version(self):
        with self.assertRaises(AssertionError):boot_ledger(self.good+'H2_PAL_MQTT_BOOT id='+('d'*32+'-'+'e'*16)+' version=v2\n',self.ids,'v1')
    def test_late_new_nonce(self):
        with self.assertRaises(AssertionError):boot_ledger(self.good+'H2_PAL_MQTT_BOOT id='+('d'*32+'-'+'e'*16)+' version=v1\n',self.ids,'v1')
    def test_previous_boot_replay(self):
        with self.assertRaises(AssertionError):boot_ledger(self.good,self.ids,'v1',self.execution)
    def test_missing_p2_valid(self):
        before=dict(result='OK',code='0',device_uid='uid',board='board',target='bk7258',partition_1_valid='1',partition_1_role='loader',partition_1_package_checksum='a'*64,partition_1_image_checksum='b'*64)
        after={**before,**dict(active_role='app',running_partition='2',next_partition='2',stage_valid='0',last_result='0',active_version='v1',active_checksum='c'*64,partition_2_role='app',partition_2_version='v1',partition_2_image_checksum='c'*64,partition_2_package_checksum='d'*64)}
        manifest=dict(board='board',target='bk7258',version='v1',image_sha256='c'*64)
        with self.assertRaises(AssertionError):status_preserved(before,after,manifest,'d'*64,'uid')
        after['partition_2_valid']='0'
        with self.assertRaises(AssertionError):status_preserved(before,after,manifest,'d'*64,'uid')
        after['partition_2_valid']='1';status_preserved(before,after,manifest,'d'*64,'uid')
    def test_equal_truncated_dump(self):
        status=dict(result='OK',code='0',stored_bytes='8',blank='0')
        with self.assertRaises(AssertionError):coredump_preserved(status,status,b'four',b'four')
        self.assertIsNotNone(coredump_preserved(status,status,b'12345678',b'12345678'))
if __name__=='__main__':unittest.main()
