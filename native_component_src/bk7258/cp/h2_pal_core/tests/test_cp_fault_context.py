"""Run the sealed CP exception entries before blocked SDK dump guards."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def function(source, name):
    source = re.sub(
        r'__attribute__\(\(section\("\.iram"\)(?:, noinline)?\)\)\s*',
        '', source)
    match = re.search(r'^(?:static\s+)?void\s+' + name +
                      r'\s*\([^;{]*\)\s*\{', source, re.M)
    assert match, 'missing CP SDK function: ' + name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--locator', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    component = repo / 'native_component_src/bk7258/cp/h2_pal_core'
    locator = json.loads(args.locator.read_text()) if args.locator else None
    sdk = args.sdk or Path(locator['paths']['checkout'] if locator
                           else os.environ['BK7258_PATH'])
    assert subprocess.check_output(
        ['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip() == (
            repo / 'tools/bazel/native_versions/bk7258_sdk_commit.txt').read_text().strip()
    assert not subprocess.check_output(
        ['git', '-C', str(sdk), 'status', '--porcelain=v1', '--untracked-files=no'],
        text=True)
    trap = sdk / 'cp/middleware/arch/cm33/trap_base.c'
    startup = sdk / 'cp/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Source/startup_bk7236.c'
    fingerprints = {p: hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in (trap, startup)}
    cc, cmake = shutil.which('cc'), shutil.which('cmake')
    assert cc and cmake
    with tempfile.TemporaryDirectory(prefix='h2-cp-fault-') as temporary:
        work = Path(temporary)
        baseline_trap = work / 'baseline_trap.c'
        baseline_startup = work / 'baseline_startup.c'
        for output, relative in [(baseline_trap, 'cp/middleware/arch/cm33/trap_base.c'), (baseline_startup, 'cp/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Source/startup_bk7236.c')]:
            output.write_text(subprocess.check_output(['git','-C',str(sdk),'show','9020466789460701a62a823deb4a98edd6ea0e5d:'+relative],text=True))
        generated = [trap, startup]
        headers = work / 'headers'
        headers.mkdir()
        shutil.copy2(sdk / 'cp/middleware/arch/cm33/include/stack_base.h', headers / 'stack_base.h')
        probe = (component / 'tests/cp_fault_context_probe.c').read_text()
        profiles = list(itertools.product(
            ('nmi-guard', 'nmi-ap', 'nmi-reboot', 'nmi-normal', 'trap-guard',
             'trap-normal', 'invalid-frame', 'uart-full', 'nested', 'reset-budget'),
            (0xfffffff9, 0xfffffffd, 0xffffffe9, 0xffffffed), (0, 1)))
        positives, negatives = 0, 0
        for name, inputs in [('original', [baseline_trap, baseline_startup]), ('corrected', generated)]:
            trap_source, startup_source = (p.read_text() for p in inputs)
            for old, new in [('bk_capture_fault_frame', 'h2_bk_cp_capture_frame'), ('bk_fault_capture', 'h2_bk_cp_fault_capture'), ('bk_fault_context_enter', 'h2_bk_cp_fault_context_enter'), ('bk_fault_context_observe', 'h2_bk_cp_fault_context_observe')]:
                trap_source = trap_source.replace(old, new)
                startup_source = startup_source.replace(old, new)
            names = (['h2_bk_cp_capture_frame', 'h2_bk_cp_fault_capture']
                     if name == 'corrected' else []) + ['dump_context', 'user_except_handler_ex']
            functions = 'static void test_fault_persist(uint32_t reason, const SAVED_CONTEXT *context) { (void)reason; (void)context; }\nvoid (*bk_fault_context_persist)(uint32_t, const SAVED_CONTEXT*) = test_fault_persist;\n' + '\n'.join(function(trap_source, symbol) for symbol in names)
            functions += '\n' + function(startup_source, 'user_nmi_handler')
            functions = functions.replace('((uint32_t *)msp)', 'test_context_stack(msp)')
            functions = functions.replace('((uint32_t *)stack_pointer)', 'test_context_stack(stack_pointer)')
            unit = work / (name + '.c')
            unit.write_text(probe.replace('/* SDK_FUNCTIONS */', functions))
            for dump in (0, 1):
                binary = work / (name + str(dump))
                command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-Wno-unused-parameter', '-Wno-unused-function',
                           '-DH2_BK_CP_FAULT_HOST_TEST=1', '-DH2_TEST_DUMP=' + str(dump),
                           '-DH2_TEST_CORRECTED=' + str(int(name == 'corrected')),
                           '-I' + str(headers), '-I' + str(component / 'tests/cp_fault_sdk'),
                           '-I' + str(component / 'src'), str(unit),
                           str(component / 'src/h2_bk_cp_fault_context.c'), '-o', str(binary)]
                if args.sanitize and name == 'corrected':
                    command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                subprocess.run(command, check=True, timeout=60)
                for mode, exc, align in profiles:
                    if name == 'original' and (exc != 0xfffffffd or align != 0):
                        continue
                    result = subprocess.run([str(binary), mode, hex(exc), str(align)],
                                            capture_output=True, text=True, timeout=10)
                    if name == 'corrected':
                        assert result.returncode == 0, (dump, mode, hex(exc), align, result.stderr)
                        positives += 1
                    else:
                        assert result.returncode != 0, (dump, mode, exc, align)
                        negatives += 1
        for path, fingerprint in fingerprints.items():
            assert hashlib.sha256(path.read_bytes()).hexdigest() == fingerprint
        print(f'CP early context: {positives} SDK profiles pass; {negatives} original negative controls fail')


if __name__ == '__main__':
    main()
