"""Run selected SDK Flash locks with deterministic AP/APX/CP contention."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^static void " + name + r"\(void\)\s*\{", source, re.M)
    assert match, name
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
    sdk = args.sdk or Path(locator["paths"]["checkout"] if locator else os.environ["BK7258_PATH"])
    expected = (repo / "tools/bazel/native_versions/bk7258_sdk_commit.txt").read_text().strip()
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    assert actual == expected, (actual, expected)
    assert not subprocess.check_output(
        ["git", "-C", str(sdk), "status", "--porcelain", "--untracked-files=no"], text=True)
    original = sdk / "ap/middleware/driver/flash/flash_shared_lock.c"
    before = hashlib.sha256(original.read_bytes()).hexdigest()
    driver = (original.parent / "flash_driver.c").read_text()
    driver_lock = function(driver, "flash_lock") + "\n" + function(driver, "flash_unlock")
    probe = (component / "tests/flash_lock_order_probe.c").read_text()
    cc, cmake = shutil.which("cc"), shutil.which("cmake")
    assert cc and cmake
    with tempfile.TemporaryDirectory(prefix="h2-flash-lock-order-") as temporary:
        work = Path(temporary)
        (work / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
project(flash_lock C)
set(CONFIG_FLASH ON)
set(CONFIG_FREERTOS_SMP ON)
add_library(driver STATIC "${BK_ARMINO_PATH}/middleware/driver/flash/flash_shared_lock.c")
function(armino_component_get_property output component property)
  set(${output} driver PARENT_SCOPE)
endfunction()
include("${H2_FLASH_COMPONENT}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt"
  CONTENT "$<JOIN:$<TARGET_PROPERTY:driver,SOURCES>,\\n>")
''')
        for enabled in ("OFF", "ON"):
            subprocess.run([
                cmake, "-S", str(work), "-B", str(work / enabled.lower()),
                "-DBK_ARMINO_PATH=" + str(sdk / "ap"),
                "-DCONFIG_FLASH_CP_AP_DIRECT_ACCESS=" + enabled,
                "-DH2_FLASH_COMPONENT=" + str(component / "src/h2_bk_flash_lock_component.cmake"),
            ], check=True, capture_output=True, text=True)
        assert Path((work / "off/selected.txt").read_text().strip()) == original
        corrected = Path((work / "on/selected.txt").read_text().strip())
        assert corrected != original
        repeated = subprocess.run([
            cmake, "-DH2_BK_FLASH_LOCK_INPUT=" + str(corrected),
            "-DH2_BK_FLASH_LOCK_OUTPUT=" + str(work / "repeated.c"),
            "-P", str(component / "src/h2_bk_flash_lock_order.cmake"),
        ], capture_output=True, text=True)
        assert repeated.returncode != 0 and "source changed" in repeated.stderr
        for name, path in [("original", original), ("corrected", corrected)]:
            shared = re.sub(r"^#include[^\n]*\n", "", path.read_text(), flags=re.M)
            shared = shared.replace('__asm volatile("dmb sy" ::: "memory");',
                                    "atomic_thread_fence(memory_order_seq_cst);")
            # MMIO fields have independent core writers. Use atomic host
            # storage for the hardware's ordered volatile word accesses.
            shared = shared.replace("volatile uint32_t", "_Atomic uint32_t")
            unit, binary = work / (name + ".c"), work / name
            unit.write_text(probe.replace("/* SDK_SHARED_LOCK */", shared)
                            .replace("/* SDK_DRIVER_LOCK */", driver_lock))
            command = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-pthread", str(unit), "-o", str(binary)]
            if args.sanitize and name == "corrected":
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)
            run = subprocess.run([str(binary), "overlap"], capture_output=True, text=True, timeout=10)
            if name == "original":
                assert run.returncode == 86 and "LOCK_ORDER_CYCLE" in run.stderr, run
            else:
                assert run.returncode == 0, run.stdout + run.stderr
                print(run.stdout, end="")
    assert hashlib.sha256(original.read_bytes()).hexdigest() == before
    print(json.dumps({"result": "PASS", "sdk_head": actual,
                      "original_negative_control": "lock cycle",
                      "corrected": "all workers joined and locks released",
                      "sanitize": args.sanitize}, sort_keys=True))


if __name__ == "__main__":
    main()
