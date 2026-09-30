"""Use H2Loader directly to qualify a fresh upgrade and independent App boot.

No Bazel invocation is performed here. BUILD owns package/CLI/registry inputs;
port and UID are explicit runtime fixture identities, never guessed.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import signal
import time
import zlib

def fields(text):
    return dict(re.findall(r'(\w+)=([^\s]+)', text))

def boot_ledger(text, ids, version, previous=None):
    observations=list(re.finditer(r'H2_ATOMIC_EXECUTION (\{[^\r\n]+\})',text))
    for index,marker in enumerate(observations):
        info=json.loads(marker.group(1))
        if info.get('version')!=version or info.get('execution')==previous:continue
        assert re.fullmatch('[0-9a-f]{32}',info.get('execution','')), 'invalid execution identity'
        assert info.get('cleanup')==0 and info.get('confirm')==0, 'not admitted'
        end=observations[index+1].start() if index+1<len(observations) else len(text)
        current=text[marker.end():end]
        match=re.search(r'H2_ATOMIC_REPORT (\{[^\r\n]+\})',current)
        if not match:continue
        report=json.loads(match.group(1))
        rows=[json.loads(row) for row in re.findall(r'H2_ATOMIC_CASE (\{[^\r\n]+\})',current[:match.start()])]
        assert [(r['placement'],r['id']) for r in rows]==[(p,i) for p in ['internal','psram-wrapper'] for i in ids], 'case ledger'
        assert all(r['version']==version and r['status']=='PASS' and r['rc']==0 for r in rows), 'failed case'
        for key,value in dict(version=version,passed=56,failed=0,not_run=0,workers_started=20,workers_joined=20,qualified=1,teardown=0).items():
            assert report.get(key)==value,(key,report)
        assert not re.search(r'panic|hard fault|assert failed|H2_ATOMIC_FAIL|verdict=FAIL',current,re.I), 'boot failure'
        report['execution']=info['execution'];report['cleanup']=info['cleanup'];report['confirm']=info['confirm']
        return report
    raise AssertionError('missing complete fresh execution receipt')

def main():
    package, cli, registry, target = map(str,sys.argv[1:])
    port=os.environ.get('H2_ATOMIC_DEVICE_PORT')
    uid=os.environ.get('H2_ATOMIC_DEVICE_UID')
    if not port or not uid: raise ValueError('explicit H2_ATOMIC_DEVICE_PORT and H2_ATOMIC_DEVICE_UID required')
    ids=re.findall(r'H2_ATOMIC_CASE\("([^"]+)"',Path(registry).read_text())
    assert len(ids)==28 and len(set(ids))==28
    original=Path(package).read_bytes()
    with tarfile.open(fileobj=io.BytesIO(zlib.decompress(original))) as tar:
        manifest=dict(line.split('=',1) for line in tar.extractfile('manifest').read().decode().splitlines() if line)
        image_members=[m for m in tar.getmembers() if m.name.startswith('app/') and m.isfile()]
        assert len(image_members)==1, 'managed App image'
        image_bytes=tar.extractfile(image_members[0]).read()
        assert manifest.get('role')=='app' and manifest.get('target')==target
        assert hashlib.sha256(image_bytes).hexdigest()==manifest['image_sha256']
        assert len(image_bytes)==int(manifest['image_size'])
    version=manifest['version']
    output=Path(os.environ['TEST_UNDECLARED_OUTPUTS_DIR']);output.mkdir(exist_ok=True,parents=True)
    # Native CLI mounts /tmp and home. Keep readable CLI payloads under /tmp.
    with tempfile.TemporaryDirectory(prefix='atomic-device-',dir='/tmp') as directory:
        local=Path(directory);image=local/'atomic.update.tar.zlib';image.write_bytes(original)
        index=0
        def run(label,*args,timeout=180,monitor=False,previous=None):
            nonlocal index
            command=[cli,'--no-ble','--port',port,'--transport','iostreamikcp','--wait-timeout','150',*args]
            log_path=output/f'{index:02d}-{label}.log';index+=1
            observed=False
            with log_path.open('w') as log:
                process=subprocess.Popen(command,text=True,stdout=log,stderr=subprocess.STDOUT)
                try:
                    if monitor:
                        deadline=time.monotonic()+timeout
                        while time.monotonic()<deadline:
                            text=log_path.read_text(errors='replace')
                            try:
                                boot_ledger(text,ids,version,previous)
                            except (AssertionError,ValueError):
                                pass
                            else:
                                observed=True
                                process.send_signal(signal.SIGINT)
                                break
                            if process.poll() is not None:break
                            time.sleep(0.1)
                        else:
                            raise TimeoutError('fresh admitted Atomic execution ledger not observed')
                    code=process.wait(timeout=timeout if not monitor else 10)
                except BaseException:
                    if process.poll() is None:
                        process.send_signal(signal.SIGINT)
                        try:process.wait(timeout=10)
                        except subprocess.TimeoutExpired:process.kill();process.wait()
                    raise
            text=log_path.read_text(errors='replace')
            # Native monitor can report either graceful exit or conventional
            # SIGINT status. This is allowed only after the full oracle passed.
            if code and not (observed and code in (130,-signal.SIGINT)):
                raise RuntimeError((label,code,text[-2500:]))
            return text
        before=fields(run('before-status','status'))
        assert before.get('device_uid')==uid, 'fixture UID mismatch'
        resume=before.get('stage_valid')=='1'
        if resume:
            expected_stage=dict(stage_package_checksum=hashlib.sha256(original).hexdigest(),stage_version=version,
                stage_image_checksum=manifest['image_sha256'],stage_role='app',stage_board=manifest['board'],stage_target=target)
            assert all(before.get(k)==v for k,v in expected_stage.items()), 'nonempty Stage belongs to a different package'
        else:assert before.get('stage_valid')=='0', 'invalid Stage state'
        p1={k:v for k,v in before.items() if k.startswith('partition_1_')}
        before_coredump=fields(run('before-coredump-status','coredump','status'))
        assert before_coredump.get('result')=='OK' and before_coredump.get('code')=='0'
        before_dump=local/'before.bin'
        has_dump=int(before_coredump['stored_bytes'])>0
        if has_dump:
            assert before_coredump.get('blank')=='0'
            run('before-coredump','coredump','dump','--output',str(before_dump))
            shutil.copy2(before_dump,output/'coredump-before.bin')
        else:
            assert before_coredump.get('blank')=='1', 'inconsistent empty dump'

        if not resume:run('send','send','--file',str(image),timeout=300)
        # BK's managed flash installation of the full AP/CP image can take
        # several minutes. This is a transport/flash bound, not worker timing.
        upgrade=run('upgrade','reboot','upgrade','--monitor',timeout=600,monitor=True)
        first=boot_ledger(upgrade,ids,version)
        normal=run('normal-boot','reboot','app','--monitor',monitor=True,previous=first['execution'])
        second=boot_ledger(normal,ids,version,first['execution'])
        final=fields(run('after-status','status'))
        assert final.get('device_uid')==uid
        assert all(final.get(k)==v for k,v in p1.items()), 'Loader P1 changed'
        for key,val in dict(active_role='app',running_partition='2',next_partition='2',stage_valid='0',last_result='0',active_version=version,
            active_checksum=manifest['image_sha256'],partition_2_version=version,partition_2_image_checksum=manifest['image_sha256'],
            partition_2_package_checksum=hashlib.sha256(original).hexdigest()).items():
            assert final.get(key)==val,(key,final)
        after_coredump=fields(run('after-coredump-status','coredump','status'))
        assert before_coredump==after_coredump,'coredump status changed'
        after_dump=local/'after.bin'
        if has_dump:
            run('after-coredump','coredump','dump','--output',str(after_dump))
            shutil.copy2(after_dump,output/'coredump-after.bin')
            assert before_dump.read_bytes()==after_dump.read_bytes(),'coredump changed'

        receipt=dict(target=target,port=port,uid=uid,package_sha256=hashlib.sha256(original).hexdigest(),manifest=manifest,
            cli_sha256=hashlib.sha256(Path(cli).read_bytes()).hexdigest(),registry_sha256=hashlib.sha256(Path(registry).read_bytes()).hexdigest(),
            managed_boot=first,normal_boot=second,final_status=final,coredump_status=after_coredump,coredump_sha256=hashlib.sha256(after_dump.read_bytes()).hexdigest() if has_dump else None,observed_coredump_bytes=len(after_dump.read_bytes()) if has_dump else None)
        (output/'qualification.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print(f'Atomic {target}: 56/56 PASS on managed and independent App boots; cleanup=0, P1/Stage/coredump preserved')
if __name__=='__main__':main()
