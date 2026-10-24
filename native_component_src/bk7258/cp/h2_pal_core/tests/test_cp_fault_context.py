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
        (work / 'keep.c').write_text('int keep_source(void) { return 0; }\n')
        (work / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.18)
project(cp_fault C)
set(trap "${H2_BK_CP_SDK_ROOT}/middleware/arch/cm33/trap_base.c")
set(startup "${H2_BK_CP_SDK_ROOT}/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Source/startup_bk7236.c")
add_library(arch STATIC "${trap}" keep.c)
add_library(startup_lib STATIC "${startup}" keep.c)
add_library(core STATIC keep.c)
set(COMPONENT_LIB core)
foreach(original "${trap}" "${startup}")
  set_source_files_properties("${original}" PROPERTIES
    COMPILE_OPTIONS "-DH2_KEEP_OPTIONS=1"
    COMPILE_DEFINITIONS "H2_KEEP_DEFINITION=1"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
function(armino_component_get_property output component property)
  if(NOT CONFIG_H2_BK_PLATFORM_CP_FAULT_TRACE OR NOT CONFIG_ARCH_CM33 OR CONFIG_SOC_SMP)
    message(FATAL_ERROR "Disabled CP diagnostics requested an SDK component")
  endif()
  if(component STREQUAL "cm33")
    set(${output} arch PARENT_SCOPE)
  elseif(component STREQUAL "cmsis")
    set(${output} startup_lib PARENT_SCOPE)
  else()
    message(FATAL_ERROR "Unexpected CP SDK component")
  endif()
endfunction()
include("${H2_CP_COMPONENT}")
foreach(library arch startup_lib)
  get_target_property(selected ${library} SOURCES)
  list(REMOVE_ITEM selected keep.c)
  file(WRITE "${CMAKE_BINARY_DIR}/${library}-selected.txt" "${selected}")
  foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES)
    get_source_file_property(value "${selected}" TARGET_DIRECTORY ${library} ${property})
    file(WRITE "${CMAKE_BINARY_DIR}/${library}-${property}.txt" "${value}")
  endforeach()
endforeach()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/link.txt"
  CONTENT "$<JOIN:$<TARGET_PROPERTY:core,INTERFACE_LINK_OPTIONS>,\\n>")
''')
        generated = None
        for trace, arm, smp in itertools.product(('OFF', 'ON'), repeat=3):
            build = work / (trace + '-' + arm + '-' + smp)
            subprocess.run([cmake, '-S', str(work), '-B', str(build),
                            '-DH2_BK_CP_SDK_ROOT=' + str(sdk / 'cp'),
                            '-DCONFIG_H2_BK_PLATFORM_CP_FAULT_TRACE=' + trace,
                            '-DCONFIG_ARCH_CM33=' + arm, '-DCONFIG_SOC_SMP=' + smp,
                            '-DH2_CP_COMPONENT=' + str(component / 'src/h2_bk_cp_fault_context_component.cmake')],
                           capture_output=True, text=True, check=True)
            enabled = trace == arm == 'ON' and smp == 'OFF'
            paths = [Path((build / (name + '-selected.txt')).read_text())
                     for name in ('arch', 'startup_lib')]
            if enabled:
                assert all(p.is_file() and p.parent == build for p in paths)
                assert (build / 'link.txt').read_text() == '-Wl,-u,h2_bk_cp_fault_context_enter'
                generated = paths
            else:
                assert paths == [trap, startup]
                assert (build / 'link.txt').read_text() == ''
            for name in ('arch', 'startup_lib'):
                for prop, expected in [('COMPILE_OPTIONS', '-DH2_KEEP_OPTIONS=1'),
                                       ('COMPILE_DEFINITIONS', 'H2_KEEP_DEFINITION=1'),
                                       ('INCLUDE_DIRECTORIES', str(work))]:
                    assert (build / (name + '-' + prop + '.txt')).read_text() == expected
        assert generated is not None
        changed = work / 'changed.c'
        changed.write_text(trap.read_text() + '\n/* SDK drift */\n')
        output = work / 'rejected.c'
        result = subprocess.run([cmake, '-DH2_BK_CP_TRAP_INPUT=' + str(changed),
                                 '-DH2_BK_CP_STARTUP_INPUT=' + str(startup),
                                 '-DH2_BK_CP_TRAP_OUTPUT=' + str(output),
                                 '-DH2_BK_CP_STARTUP_OUTPUT=' + str(work / 'rejected-startup.c'),
                                 '-P', str(component / 'src/h2_bk_cp_fault_context_overlay.cmake')],
                                capture_output=True, text=True)
        assert result.returncode != 0 and not output.exists()

        headers = work / 'headers'
        headers.mkdir()
        shutil.copy2(sdk / 'cp/middleware/arch/cm33/include/stack_base.h', headers / 'stack_base.h')
        probe = (component / 'tests/cp_fault_context_probe.c').read_text()
        profiles = list(itertools.product(
            ('nmi-guard', 'nmi-ap', 'nmi-reboot', 'nmi-normal', 'trap-guard',
             'trap-normal', 'invalid-frame', 'uart-full', 'nested'),
            (0xfffffff9, 0xfffffffd, 0xffffffe9, 0xffffffed), (0, 1)))
        positives, negatives = 0, 0
        for name, inputs in [('original', [trap, startup]), ('corrected', generated)]:
            trap_source, startup_source = (p.read_text() for p in inputs)
            names = (['h2_bk_cp_capture_frame', 'h2_bk_cp_fault_capture']
                     if name == 'corrected' else []) + ['dump_context', 'user_except_handler_ex']
            functions = '\n'.join(function(trap_source, symbol) for symbol in names)
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
