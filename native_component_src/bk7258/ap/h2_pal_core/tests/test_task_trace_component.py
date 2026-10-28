"""Execute the SDK-owned kernel snapshot and GizOS output ownership regression."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--locator', type=Path)
    parser.add_argument('--pointer-names', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    locator = json.loads(args.locator.read_text()) if args.locator else None
    sdk = args.sdk or Path(locator['paths']['checkout'])
    expected = (repo / 'tools/bazel/native_versions/bk7258_sdk_commit.txt').read_text().strip()
    assert subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()==expected
    assert not subprocess.check_output(['git','-C',str(sdk),'status','--porcelain','--untracked-files=no'],text=True)
    component = repo / 'native_component_src/bk7258/ap/h2_pal_core'
    kernel = sdk / 'ap/components/os_source/freertos_smp_v2p0/FreeRTOS-Kernel'
    header = kernel / 'bk_task_snapshot_impl.h'
    digest = hashlib.sha256(header.read_bytes()).hexdigest()
    cc, cxx = shutil.which('cc'), shutil.which('c++')
    assert cc and cxx
    with tempfile.TemporaryDirectory(prefix='bk-task-snapshot-') as temporary:
        work=Path(temporary)
        includes=[component/'include',component/'src',component/'tests/task_trace_sdk',repo/'libs/pal/include',sdk/'ap/include',kernel]
        flags=['-DH2_BK_TASK_TRACE_HOST_TEST=1','-DCONFIG_USE_STATIC_TASK_NAME='+str(int(args.pointer_names))]
        flags += ['-I'+str(p) for p in includes]
        common=['-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        obj=work/'trace.o';binary=work/'probe'
        subprocess.run([cc,'-std=c11',*common,*flags,'-c',str(component/'src/h2_bk_task_trace.c'),'-o',str(obj)],check=True)
        subprocess.run([cxx,'-std=c++17',*common,*flags,'-pthread',str(component/'tests/test_h2_bk_task_trace.cpp'),str(obj),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=90)
    assert hashlib.sha256(header.read_bytes()).hexdigest()==digest
    print(json.dumps({'result':'PASS','sdk_commit':expected,'pointer_names':args.pointer_names,'production_snapshot_sha256':digest,'sanitized':True}))


if __name__=='__main__':
    main()
