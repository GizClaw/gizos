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
import zlib

def fields(text):
    return dict(re.findall(r'(\w+)=([^\s]+)', text))

def boot_ledger(text, ids, version):
    marker = 'H2_ATOMIC_BOOT version=' + version
    assert text.count(marker) == 1, 'missing/repeated new boot marker'
    current = text.split(marker, 1)[1]
    match = re.search(r'H2_ATOMIC_REPORT (\{[^\r\n]+\})', current)
    assert match, 'missing completed ledger'
    report = json.loads(match.group(1))
    rows = [json.loads(row) for row in re.findall(r'H2_ATOMIC_CASE (\{[^\r\n]+\})', current[:match.start()])]
    assert [(row['placement'], row['id']) for row in rows] == [(p, i) for p in ['internal','psram-wrapper'] for i in ids], 'case ledger'
    assert all(row['version']==version and row['status']=='PASS' and row['rc']==0 for row in rows), 'failed case'
    for key, value in dict(version=version,passed=56,failed=0,not_run=0,workers_started=20,workers_joined=20,qualified=1,teardown=0).items():
        assert report.get(key)==value, (key,report)
    assert 'H2_ATOMIC_CLEANUP rc=0' in current and 'H2_ATOMIC_READY rc=0 confirm=0' in current
    assert not re.search(r'panic|hard fault|assert failed|H2_ATOMIC_FAIL|verdict=FAIL',current,re.I), 'boot failure'
    return report

def main():
    package, cli, registry, target = map(str,sys.argv[1:])
    port=os.environ.get('H2_ATOMIC_DEVICE_PORT')
    uid=os.environ.get('H2_ATOMIC_DEVICE_UID')
    if not port or not uid: raise ValueError('explicit H2_ATOMIC_DEVICE_PORT and H2_ATOMIC_DEVICE_UID required')
    ids=re.findall(r'H2_ATOMIC_CASE\("([^"]+)"',Path(registry).read_text())
    assert len(ids)==28 and len(set(ids))==28
    original=Path(package).read_bytes()
    with tarfile.open(fileobj=io.BytesIO(zlib.decompress(original))) as tar:
        member=next(m for m in tar.getmembers() if m.name.endswith('manifest.json'))
        manifest=json.load(tar.extractfile(member))
    version=manifest['version']
    output=Path(os.environ['TEST_UNDECLARED_OUTPUTS_DIR']);output.mkdir(exist_ok=True,parents=True)
    # Native CLI mounts /tmp and home. Keep readable CLI payloads under /tmp.
    with tempfile.TemporaryDirectory(prefix='atomic-device-',dir='/tmp') as directory:
        local=Path(directory);image=local/'atomic.update.tar.zlib';image.write_bytes(original)
        index=0
        def run(label,*args,timeout=180):
            nonlocal index
            command=[cli,'--no-ble','--port',port,'--transport','iostreamikcp','--wait-timeout','150',*args]
            try:
                completed=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
            except subprocess.TimeoutExpired as error:
                partial=error.stdout or ''
                if isinstance(partial,bytes):partial=partial.decode('utf-8',errors='replace')
                (output/f'{index:02d}-{label}.log').write_text(partial)
                raise
            (output/f'{index:02d}-{label}.log').write_text(completed.stdout);index+=1
            if completed.returncode:raise RuntimeError((label,completed.returncode,completed.stdout[-2500:]))
            return completed.stdout
        before=fields(run('before-status','status'))
        assert before.get('device_uid')==uid and before.get('stage_valid')=='0', 'fixture UID/Stage mismatch'
        p1={k:v for k,v in before.items() if k.startswith('partition_1_')}
        run('before-coredump-status','coredump','status')
        before_dump=local/'before.bin';run('before-coredump','coredump','dump','--output',str(before_dump))
        shutil.copy2(before_dump,output/'coredump-before.bin')
        run('send','send','--file',str(image),timeout=300)
        upgrade=run('upgrade','--ready','H2_ATOMIC_READY rc=0 confirm=0','reboot','upgrade','--monitor')
        first=boot_ledger(upgrade,ids,version)
        normal=run('normal-boot','--ready','H2_ATOMIC_READY rc=0 confirm=0','reboot','app','--monitor')
        second=boot_ledger(normal,ids,version)
        final=fields(run('after-status','status'))
        assert final.get('device_uid')==uid
        assert all(final.get(k)==v for k,v in p1.items()), 'Loader P1 changed'
        for key,val in dict(active_role='app',running_partition='2',next_partition='2',stage_valid='0',last_result='0',active_version=version,
            active_checksum=manifest['image_sha256'],partition_2_version=version,partition_2_image_checksum=manifest['image_sha256'],
            partition_2_package_checksum=hashlib.sha256(original).hexdigest()).items():
            assert final.get(key)==val,(key,final)
        run('after-coredump-status','coredump','status')
        after_dump=local/'after.bin';run('after-coredump','coredump','dump','--output',str(after_dump))
        shutil.copy2(after_dump,output/'coredump-after.bin')
        assert before_dump.read_bytes()==after_dump.read_bytes(),'coredump changed'
        receipt=dict(target=target,port=port,uid=uid,package_sha256=hashlib.sha256(original).hexdigest(),manifest=manifest,
            cli_sha256=hashlib.sha256(Path(cli).read_bytes()).hexdigest(),registry_sha256=hashlib.sha256(Path(registry).read_bytes()).hexdigest(),
            managed_boot=first,normal_boot=second,final_status=final,coredump_sha256=hashlib.sha256(after_dump.read_bytes()).hexdigest())
        (output/'qualification.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print(f'Atomic {target}: 56/56 PASS on managed and independent App boots; cleanup=0, P1/Stage/coredump preserved')
if __name__=='__main__':main()
