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
        baseline = work / "baseline_trap.c"
        baseline.write_text(subprocess.check_output(["git", "-C", str(sdk), "show", "9020466789460701a62a823deb4a98edd6ea0e5d:ap/middleware/arch/cm33/trap_base_smp.c"], text=True))
        generated = trap
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
        for name, source in (("original", baseline), ("corrected", generated)):
            symbols = (["h2_bk_capture_fault_context"] if name == "corrected" else []) + [
                "dump_context", "user_except_handler", "store_cpu_regs",
                "bk_system_dump", "user_except_handler_ex"]
            text = source.read_text()
            for old, new in [("bk_capture_fault_context", "h2_bk_capture_fault_context"), ("bk_fault_context_enter", "h2_bk_fault_context_enter"), ("bk_fault_context_observe", "h2_bk_fault_context_observe")]:
                text = text.replace(old, new)
            functions = "\n".join(function(text, symbol) for symbol in symbols)
            functions = "static void test_fault_persist(uint32_t reason, const SAVED_CONTEXT *context) { (void)reason; (void)context; }\nvoid (*bk_fault_context_persist)(uint32_t, const SAVED_CONTEXT*) = test_fault_persist;\n" + functions
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
