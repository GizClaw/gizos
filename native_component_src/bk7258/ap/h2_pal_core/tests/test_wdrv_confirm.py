"""Execute the actual corrected SDK confirmation queue with controlled RTOS calls."""
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
    match = re.search(r"^(?:int|void)\s+" + name + r"\s*\([^;{]*\)\s*\{", source, re.M)
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
    sdk = args.sdk or Path(locator["paths"]["checkout"] if locator else os.environ["BK7258_PATH"])
    expected = (repo / "tools/bazel/native_versions/bk7258_sdk_commit.txt").read_text().strip()
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    assert actual == expected, (actual, expected)
    assert not subprocess.check_output(
        ["git", "-C", str(sdk), "status", "--porcelain=v1", "--untracked-files=no"], text=True)
    driver = sdk / "ap/components/bk_wifi_driver"
    original = driver / "wdrv_tx.c"
    before = hashlib.sha256(original.read_bytes()).hexdigest()
    rx = (driver / "wdrv_rx.c").read_text()
    lists = (driver / "wdrv_co_list.c").read_text()
    probe = (component / "tests/wdrv_confirm_probe.c").read_text()
    cc, cmake = shutil.which("cc"), shutil.which("cmake")
    assert cc and cmake, "a native C compiler and CMake are required"
    with tempfile.TemporaryDirectory(prefix="h2-wdrv-confirm-") as temporary:
        work = Path(temporary)
        # Exercise the real native selection with the AP's radio-disabled
        # profile. Compiling the generator directly would miss a wrong guard.
        (work / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
project(wdrv_confirm C)
set(CONFIG_WIFI_ENABLE OFF)
add_library(driver STATIC "${BK_ARMINO_PATH}/components/bk_wifi_driver/wdrv_tx.c")
function(armino_component_get_property output component property)
  if(NOT CONFIG_WIFI_VNET_CONTROLLER)
    message(FATAL_ERROR "WiFi-disabled image requested WDRV")
  endif()
  set(${output} driver PARENT_SCOPE)
endfunction()
include("${H2_WDRV_COMPONENT}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt"
  CONTENT "$<JOIN:$<TARGET_PROPERTY:driver,SOURCES>,\\n>")
''')
        for enabled in ("OFF", "ON"):
            build = work / enabled.lower()
            subprocess.run([cmake, "-S", str(work), "-B", str(build),
                            "-DBK_ARMINO_PATH=" + str(sdk / "ap"),
                            "-DCONFIG_WIFI_VNET_CONTROLLER=" + enabled,
                            "-DH2_WDRV_COMPONENT=" + str(component / "src/h2_bk_wdrv_component.cmake")],
                           check=True, capture_output=True, text=True)
        generated = Path((work / "on/selected.txt").read_text().strip())
        repeated = subprocess.run(
            [cmake, "-DH2_BK_WDRV_INPUT=" + str(generated),
             "-DH2_BK_WDRV_OUTPUT=" + str(work / "repeated.c"), "-P",
             str(component / "src/h2_bk_wdrv_confirm.cmake")],
            capture_output=True, text=True, check=False)
        assert repeated.returncode != 0, "already-corrected SDK was accepted"
        assert "Pinned BK WDRV source changed" in repeated.stderr, repeated.stderr
        common = "\n".join([function(lists, "co_list_push_back"),
                            function(lists, "co_list_extract"),
                            function(rx, "wdrv_rx_confirm_tx_msg")])
        for name, text in (("original", original.read_text()), ("corrected", generated.read_text())):
            unit, binary = work / (name + ".c"), work / name
            unit.write_text(probe.replace("/* SDK_FUNCTIONS */",
                common + "\n" + function(text, "wdrv_tx_msg")))
            command = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-pthread", str(unit), "-o", str(binary)]
            if args.sanitize and name == "corrected":
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=60)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            if name == "original":
                assert run.returncode != 0, "unprotected SDK unexpectedly passed the concurrent queue test"
                assert "transmitted while receiver owns queue" in run.stderr, run.stderr
            else:
                assert run.returncode == 0, run.stdout + run.stderr
                print(run.stdout, end="")
    assert hashlib.sha256(original.read_bytes()).hexdigest() == before, "sealed SDK was modified"
    print(json.dumps(dict(result="PASS", sdk_commit=actual,
                          sdk_source_sha256=before, original_negative_control="FAIL as expected",
                          corrected="PASS", repeated_patch="rejected", sanitized=args.sanitize),
                     sort_keys=True))


if __name__ == "__main__":
    main()
