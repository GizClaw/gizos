"""Execute the sealed SDK's fatal entries before a blocked peer rendezvous."""
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
    match = re.search(r"^(?:static\s+)?void\s+" + name +
                      r"\s*\([^;{]*\)\s*\{", source, re.M)
    assert match, "missing SDK function: " + name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--locator", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    component = repo / "native_component_src/bk7258/ap/h2_pal_core"
    locator = json.loads(args.locator.read_text()) if args.locator else None
    sdk = args.sdk or Path(locator["paths"]["checkout"] if locator
                           else os.environ["BK7258_PATH"])
    commit = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"],
                                     text=True).strip()
    expected = (repo / "tools/bazel/native_versions/bk7258_sdk_commit.txt").read_text().strip()
    assert commit == expected, (commit, expected)
    assert not subprocess.check_output(
        ["git", "-C", str(sdk), "status", "--porcelain=v1", "--untracked-files=no"],
        text=True)
    trap = sdk / "ap/middleware/arch/cm33/trap_base_smp.c"
    shell = sdk / "ap/components/bk_cli/shell_task.c"
    fingerprints = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (trap, shell)}
    cc, cmake = shutil.which("cc"), shutil.which("cmake")
    assert cc and cmake
    with tempfile.TemporaryDirectory(prefix="h2-bk-fault-") as temporary:
        work = Path(temporary)
        (work / "keep.c").write_text("int keep_source(void) { return 0; }\n")
        (work / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.18)
project(fault_context C)
add_library(arch STATIC "${BK_ARMINO_PATH}/middleware/arch/cm33/trap_base_smp.c" keep.c)
add_library(shell STATIC "${BK_ARMINO_PATH}/components/bk_cli/shell_task.c")
add_library(core STATIC keep.c)
set(COMPONENT_LIB core)
set_source_files_properties("${BK_ARMINO_PATH}/components/bk_cli/shell_task.c"
  PROPERTIES COMPILE_OPTIONS "-DH2_KEEP_SHELL=1")
function(armino_component_get_property output component property)
  if(NOT CONFIG_H2_BK_PLATFORM_FAULT_TRACE OR NOT CONFIG_SOC_SMP OR NOT CONFIG_SHELL_ASYNCLOG)
    message(FATAL_ERROR "Disabled diagnostics requested an SDK component")
  endif()
  if(component STREQUAL "cm33")
    set(${output} arch PARENT_SCOPE)
  elseif(component STREQUAL "bk_cli")
    set(${output} shell PARENT_SCOPE)
  else()
    message(FATAL_ERROR "Unexpected SDK component")
  endif()
endfunction()
include("${H2_FAULT_COMPONENT}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt"
  CONTENT "$<JOIN:$<TARGET_PROPERTY:arch,SOURCES>,\\n>")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/link.txt"
  CONTENT "$<JOIN:$<TARGET_PROPERTY:core,INTERFACE_LINK_OPTIONS>,\\n>")
get_source_file_property(shell_options "${BK_ARMINO_PATH}/components/bk_cli/shell_task.c"
  TARGET_DIRECTORY shell COMPILE_OPTIONS)
file(WRITE "${CMAKE_BINARY_DIR}/shell-options.txt" "${shell_options}")
''')
        generated = None
        for trace, smp, asynchronous in itertools.product(("OFF", "ON"), repeat=3):
            build = work / (trace + "-" + smp + "-" + asynchronous)
            subprocess.run([cmake, "-S", str(work), "-B", str(build),
                            "-DBK_ARMINO_PATH=" + str(sdk / "ap"),
                            "-DCONFIG_H2_BK_PLATFORM_FAULT_TRACE=" + trace,
                            "-DCONFIG_SOC_SMP=" + smp,
                            "-DCONFIG_SHELL_ASYNCLOG=" + asynchronous,
                            "-DH2_FAULT_COMPONENT=" + str(component / "src/h2_bk_fault_context_component.cmake")],
                           capture_output=True, text=True, check=True)
            selected = (build / "selected.txt").read_text().splitlines()
            assert len(selected) == 2 and "keep.c" in selected, selected
            selected.remove("keep.c")
            enabled = trace == smp == asynchronous == "ON"
            if enabled:
                generated = Path(selected[0])
                assert generated.is_file() and generated.parent == build
                assert (build / "link.txt").read_text().splitlines() == [
                    "-Wl,-u,h2_bk_fault_context_observe", "-Wl,--wrap=shell_log_flush"]
            else:
                assert Path(selected[0]) == trap
                assert (build / "link.txt").read_text() == ""
            options = (build / "shell-options.txt").read_text().split(";")
            assert "-DH2_KEEP_SHELL=1" in options
            assert ("-fno-lto" in options) == enabled
        assert generated is not None
        changed = work / "changed.c"
        changed.write_text(trap.read_text() + "\n/* changed SDK */\n")
        for name, source in (("changed", changed), ("repeated", generated)):
            output = work / (name + "-out.c")
            run = subprocess.run([cmake, "-DH2_BK_FAULT_INPUT=" + str(source),
                                  "-DH2_BK_FAULT_OUTPUT=" + str(output), "-P",
                                  str(component / "src/h2_bk_fault_context_overlay.cmake")],
                                 capture_output=True, text=True, check=False)
            assert run.returncode != 0 and not output.exists(), name

        # Use the real SDK context type and assertion macro. Only hardware
        # register/memory accesses and the UART device are host shims.
        headers = work / "headers"
        (headers / "common").mkdir(parents=True)
        (headers / "components").mkdir()
        shutil.copy2(sdk / "ap/middleware/arch/cm33/include/stack_base.h", headers / "stack_base.h")
        shutil.copy2(sdk / "ap/include/common/bk_assert.h", headers / "common/bk_assert.h")
        (headers / "components/log.h").write_text(
            "void test_selected_flush(void);\n#define BK_LOG_FLUSH() test_selected_flush()\n")
        (headers / "components/system.h").write_text("/* SDK system interface shim */\n")
        probe = (component / "tests/fault_context_probe.c").read_text()
        profiles = [("fault", hex(exc), str(align), str(core))
                    for exc, align, core in itertools.product(
                        (0xfffffff9, 0xfffffffd, 0xffffffe9, 0xffffffed), (0, 1), (0, 1))]
        profiles += [(name, "0", "0", str(core))
                     for name, core in itertools.product(("assert-flush", "assert-handler"), (0, 1))]
        profiles += [("invalid-frame", "0xfffffffd", "0", str(core)) for core in (0, 1)]
        profiles += [("context-normal", hex(exc), str(align), str(core))
                     for exc, align, core in itertools.product(
                         (0xfffffff9, 0xfffffffd, 0xffffffe9, 0xffffffed), (0, 1), (0, 1))]
        flush = function(shell.read_text(), "shell_log_flush").replace(
            "void shell_log_flush(void)", "void __real_shell_log_flush(void)", 1)
        for name, source in (("original", trap), ("corrected", generated)):
            symbols = (["h2_bk_capture_fault_context"] if name == "corrected" else []) + [
                "dump_context", "user_except_handler", "store_cpu_regs",
                "bk_system_dump", "user_except_handler_ex"]
            functions = "\n".join(function(source.read_text(), symbol) for symbol in symbols)
            functions = functions.replace("((uint32_t *)msp)", "test_context_stack(msp)")
            functions = functions.replace("((uint32_t *)stack_pointer)", "test_context_stack(stack_pointer)")
            unit, binary = work / (name + ".c"), work / name
            unit.write_text(probe.replace("/* SDK_FUNCTIONS */", functions + "\n" + flush))
            command = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-DH2_BK_FAULT_HOST_TEST=1",
                       "-DH2_TEST_CORRECTED=" + str(int(name == "corrected")),
                       "-I" + str(headers), "-I" + str(component / "tests/fault_sdk"),
                       "-I" + str(component / "src"), str(unit),
                       str(component / "src/h2_bk_fault_context.c"), "-o", str(binary)]
            if args.sanitize and name == "corrected":
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=60)
            for profile in profiles:
                run = subprocess.run([str(binary), *profile], capture_output=True,
                                     text=True, timeout=10)
                if name == "original" and profile[0] != "context-normal":
                    assert run.returncode == 2 and "missing " in run.stderr, run.stdout + run.stderr
                else:
                    assert run.returncode == 0, run.stdout + run.stderr
                    print(run.stdout, end="")
    assert all(hashlib.sha256(p.read_bytes()).hexdigest() == digest
               for p, digest in fingerprints.items()), "sealed SDK was modified"
    print(json.dumps(dict(result="PASS", sdk_commit=commit, profiles=len(profiles),
                          original_negative_controls=22, normal_context_profiles=16,
                          sanitized=args.sanitize), sort_keys=True))


if __name__ == "__main__":
    main()
