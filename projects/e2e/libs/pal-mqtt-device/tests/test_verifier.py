import hashlib
import json
import tempfile
from pathlib import Path
import re
import unittest
from verify_device import boot_ledger,status_preserved,coredump_preserved,loader_status,command_receipt,after_accepted_reboot,package_binding,uart_text
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
    def test_native_non_utf8_noise_keeps_raw_hash_and_complete_records(self):
        raw=b'\x8d\x00SDK startup\nH2_LOADER_REBOOT target=upgrade result=accepted\n'+b'\x8dSDK boot noise\n'+self.good.encode()
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'managed.log';path.write_bytes(raw)
            digest=hashlib.sha256(raw).hexdigest()
            text=uart_text(path)
            self.assertIn('\ufffd',text)
            self.assertEqual(boot_ledger(after_accepted_reboot(text,'upgrade'),self.ids,'v1')['boot']['id'],self.execution)
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),digest)
    def test_non_utf8_inside_protocol_record_is_rejected(self):
        for marker in [b'H2_PAL_MQTT_BOOT ',b'H2_PAL_MQTT_RUN ',b'H2_PAL_MQTT_CASE ',b'H2_PAL_MQTT_SUMMARY ',b'H2_PAL_MQTT_READY ']:
            raw=self.good.encode().replace(marker,marker+b'\x8d',1)
            with tempfile.TemporaryDirectory() as directory:
                path=Path(directory)/'managed.log';path.write_bytes(raw)
                with self.assertRaises(AssertionError):boot_ledger(uart_text(path),self.ids,'v1')
    def test_esp_provider_cleanup_is_required(self):
        esp = self.good.replace('board=bk7258', 'board=devkit')
        with self.assertRaises(AssertionError):boot_ledger(esp,self.ids,'v1')
        for result in ['-4', '1']:
            with self.assertRaises(AssertionError):boot_ledger(esp.replace('confirm=0', 'confirm=0 provider_cleanup='+result),self.ids,'v1')
        self.assertEqual(boot_ledger(esp.replace('confirm=0', 'confirm=0 provider_cleanup=0'),self.ids,'v1')['ready']['board'], 'devkit')
    def test_real_log_prefix_fields_and_color(self):
        prefixed='\n'.join('\x1b[32mcpu=0 tick='+str(index)+' '+line+'\x1b[0m'
            for index,line in enumerate(self.good.splitlines()))
        self.assertEqual(boot_ledger(prefixed,self.ids,'v1')['boot']['id'],self.execution)
    def test_late_platform_boot(self):
        with self.assertRaises(AssertionError):boot_ledger(self.good+'H2_PAL_MQTT_PLATFORM_BOOT board=bk7258\n',self.ids,'v1')
    def test_late_rom_or_loader_startup_invalidates_ledger(self):
        for marker in ['ESP-ROM:esp32s3-20210327', 'rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)',
                       'H2_LOADER_STARTUP_EVENT event=write_partition_2 code=0']:
            with self.assertRaises(AssertionError):boot_ledger(self.good+marker+'\n',self.ids,'v1')
    def test_late_other_version(self):
        with self.assertRaises(AssertionError):boot_ledger(self.good+'H2_PAL_MQTT_BOOT id='+('d'*32+'-'+'e'*16)+' version=v2\n',self.ids,'v1')
    def test_late_new_nonce(self):
        with self.assertRaises(AssertionError):boot_ledger(self.good+'H2_PAL_MQTT_BOOT id='+('d'*32+'-'+'e'*16)+' version=v1\n',self.ids,'v1')
    def test_previous_boot_replay(self):
        with self.assertRaises(AssertionError):boot_ledger(self.good,self.ids,'v1',self.execution)
    def test_missing_p2_valid(self):
        before=dict(device_uid='uid',board='board',target='bk7258',partition_1_valid='1',partition_1_role='loader',partition_1_package_checksum='a'*64,partition_1_image_checksum='b'*64)
        after={**before,**dict(active_role='app',running_partition='2',next_partition='2',stage_valid='0',last_result='0',active_version='v1',active_checksum='c'*64,partition_2_role='app',partition_2_version='v1',partition_2_image_checksum='c'*64,partition_2_package_checksum='d'*64)}
        manifest=dict(board='board',target='bk7258',version='v1',image_sha256='c'*64)
        with self.assertRaises(AssertionError):status_preserved(before,after,manifest,'d'*64,'uid')
        after['partition_2_valid']='0'
        with self.assertRaises(AssertionError):status_preserved(before,after,manifest,'d'*64,'uid')
        after['partition_2_valid']='1';status_preserved(before,after,manifest,'d'*64,'uid')
    def test_real_status_without_synthetic_result(self):
        before_text='H2_LOADER_STATUS board=board target=bk7258 device_uid=uid partition_1_valid=1 partition_1_role=loader partition_1_package_checksum='+('a'*64)+' partition_1_image_checksum='+('b'*64)+'\n'
        after_text=before_text.rstrip()+' active_role=app running_partition=2 next_partition=2 stage_valid=0 last_result=0 active_version=v1 active_checksum='+('c'*64)+' partition_2_valid=1 partition_2_role=app partition_2_version=v1 partition_2_image_checksum='+('c'*64)+' partition_2_package_checksum='+('d'*64)+'\n'
        before=loader_status(before_text);after=loader_status(after_text)
        self.assertNotIn('result',before);self.assertNotIn('code',after)
        manifest=dict(board='board',target='bk7258',version='v1',image_sha256='c'*64)
        status_preserved(before,after,manifest,'d'*64,'uid')
        after['device_uid']='wrong'
        with self.assertRaises(AssertionError):status_preserved(before,after,manifest,'d'*64,'uid')
    def test_status_marker_required_and_unique(self):
        with self.assertRaises(AssertionError):loader_status('result=OK code=0 device_uid=uid')
        with self.assertRaises(AssertionError):loader_status('H2_LOADER_STATUS device_uid=uid\nH2_LOADER_STATUS device_uid=uid\n')
        with self.assertRaises(AssertionError):loader_status('H2_LOADER_STATUS device_uid=wrong device_uid=uid')
    def test_host_receipt_must_bind_successful_actual_command(self):
        with tempfile.TemporaryDirectory() as directory:
            log=Path(directory)/'before-status.log';log.write_text('H2_LOADER_STATUS device_uid=uid\n')
            receipt_path=Path(directory)/'before-status-receipt.json'
            receipt=dict(command=['status'],exit=0,port='/dev/fixture',started_at_utc='2026-10-04T09:58:15.167980+00:00',log_sha256=hashlib.sha256(log.read_bytes()).hexdigest())
            def write():receipt_path.write_text(json.dumps(receipt))
            with self.assertRaises(FileNotFoundError):command_receipt(log,['status'],'/dev/fixture')
            write();self.assertEqual(command_receipt(log,['status'],'/dev/fixture'),receipt)
            for key,value in [('exit',1),('exit',False),('command',['info']),('port','/dev/other'),('started_at_utc','2026-10-04T09:58:15'),('log_sha256','0'*64)]:
                original=receipt[key];receipt[key]=value;write()
                with self.assertRaises((AssertionError,ValueError)):command_receipt(log,['status'],'/dev/fixture')
                receipt[key]=original
    def test_only_post_ack_fresh_execution_is_admitted(self):
        acknowledged=self.good+'H2_LOADER_REBOOT target=upgrade result=accepted\n'
        with self.assertRaises(AssertionError):boot_ledger(after_accepted_reboot(acknowledged,'upgrade'),self.ids,'v1')
        admitted=after_accepted_reboot('H2_LOADER_REBOOT target=upgrade result=accepted\n'+self.good,'upgrade')
        self.assertEqual(boot_ledger(admitted,self.ids,'v1')['boot']['id'],self.execution)
        for text in [self.good,'H2_LOADER_REBOOT target=app result=accepted\n'+self.good,'H2_LOADER_REBOOT target=upgrade result=rejected\n'+self.good]:
            with self.assertRaises(AssertionError):after_accepted_reboot(text,'upgrade')
    def test_real_uart_ack_interrupting_old_json(self):
        interleaved='I (24488) pal-mqtt: H2_PAL_MQTT_CASE {"id":"publish-qos1","status":"PASS","detail":0,"'
        text=interleaved+'H2_LOADER_REBOOT target=app result=accepted\n'+self.good
        self.assertEqual(boot_ledger(after_accepted_reboot(text,'app'),self.ids,'v1')['boot']['id'],self.execution)
        for prefix in ['FAKE_', 'x']:
            with self.assertRaises(AssertionError):after_accepted_reboot(prefix+'H2_LOADER_REBOOT target=app result=accepted\n'+self.good,'app')
        with self.assertRaises(AssertionError):after_accepted_reboot(text+'H2_LOADER_REBOOT target=app result=accepted\n','app')
        with self.assertRaises(AssertionError):boot_ledger(after_accepted_reboot(self.good+interleaved+'H2_LOADER_REBOOT target=app result=accepted\n','app'),self.ids,'v1')
    def test_old_uart_tail_after_ack_cannot_supply_new_boot(self):
        tail='H2_PAL_MQTT_CASE {"id":"publish-qos1","status":"PASS","detail":0}\n'
        prefix='H2_LOADER_REBOOT target=app result=accepted\n'+tail
        self.assertEqual(boot_ledger(after_accepted_reboot(prefix+self.good,'app'),self.ids,'v1')['boot']['id'],self.execution)
        with self.assertRaises(AssertionError):after_accepted_reboot(prefix,'app')
        stale=self.good.split('H2_PAL_MQTT_RUN ',1)[1]
        with self.assertRaises(AssertionError):after_accepted_reboot(prefix+'H2_PAL_MQTT_RUN '+stale,'app')
    def test_monitor_requires_validated_controlled_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            log=Path(directory)/'managed.log';log.write_text('H2_LOADER_REBOOT target=upgrade result=accepted\n'+self.good)
            receipt_path=Path(directory)/'managed-receipt.json'
            receipt=dict(command=['reboot','upgrade','--monitor'],port='/dev/fixture',started_at_utc='2026-10-04T09:58:15+00:00',
                captured_at_utc='2026-10-04T10:00:15+00:00',controlled_stop=True,stop_reason='validated complete ledger',
                exit_after_capture=130,log_sha256=hashlib.sha256(log.read_bytes()).hexdigest())
            def write():receipt_path.write_text(json.dumps(receipt))
            for code in [0,130,-2]:
                receipt['exit_after_capture']=code;write()
                self.assertEqual(command_receipt(log,['reboot','upgrade','--monitor'],'/dev/fixture',True)['exit_after_capture'],code)
            for key,value in [('exit_after_capture',1),('exit_after_capture',-9),('controlled_stop',False),('stop_reason','failed ledger'),
                              ('captured_at_utc','2026-10-04T09:00:15+00:00'),('captured_at_utc','2026-10-04T10:00:15')]:
                original=receipt[key];receipt[key]=value;write()
                with self.assertRaises((AssertionError,ValueError)):command_receipt(log,['reboot','upgrade','--monitor'],'/dev/fixture',True)
                receipt[key]=original
            receipt['exit']=0;write()
            with self.assertRaises(AssertionError):command_receipt(log,['reboot','upgrade','--monitor'],'/dev/fixture',True)
    def test_equal_truncated_dump(self):
        status=dict(result='OK',code='0',stored_bytes='8',blank='0')
        with self.assertRaises(AssertionError):coredump_preserved(status,status,b'four',b'four')
        self.assertIsNotNone(coredump_preserved(status,status,b'12345678',b'12345678'))
    def test_execution_package_and_fixture_binding(self):
        manifest=dict(role='app',board='devkit',target='esp32s3',version='v1',image_size='42',image_sha256='c'*64)
        inputs=dict(ca_sha256='d'*64,epoch_ms=123,session_prefix='a'*32)
        binding=dict(package_sha256='b'*64,manifest=manifest.copy(),fixture_inputs=inputs.copy(),source_commit='e'*40,source_dirty=False)
        package_binding(binding,manifest,'b'*64,inputs)
        for key,value in [('package_sha256','f'*64),('manifest',{**manifest,'image_sha256':'f'*64}),
                          ('fixture_inputs',{**inputs,'epoch_ms':124}),('source_commit',''),('source_dirty',True)]:
            original=binding[key];binding[key]=value
            with self.assertRaises(AssertionError):package_binding(binding,manifest,'b'*64,inputs)
            binding[key]=original
if __name__=='__main__':unittest.main()
