"""Check real pinned-kernel build-copy selection and TLS option preservation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--locator", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    locator = json.loads(args.locator.read_text()) if args.locator else None
    sdk = args.sdk or Path(locator["paths"]["checkout"] if locator else os.environ["BK7258_PATH"])
    expected = (repo / "tools/bazel/native_versions/bk7258_sdk_commit.txt").read_text().strip()
    assert subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip() == expected
    assert not subprocess.check_output(["git", "-C", str(sdk), "status", "--porcelain=v1", "--untracked-files=no"], text=True)
    relative = "components/os_source/freertos_smp_v2p0/FreeRTOS-Kernel/tasks.c"
    original = sdk / "ap" / relative
    digest = hashlib.sha256(original.read_bytes()).hexdigest()
    component = repo / "native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_task_trace_component.cmake"
    cmake = shutil.which("cmake")
    assert cmake
    with tempfile.TemporaryDirectory(prefix="h2-task-trace-component-") as temporary:
        work = Path(temporary)
        (work / "keep.c").write_text("int keep(void) { return 0; }\n")
        (work / "tls.h").write_text("/* existing cleanup hook */\n")
        (work / "duplicate").mkdir()
        (work / "duplicate/tasks.c").write_text("int duplicate(void) { return 0; }\n")
        (work / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.18)
project(task_trace C)
set(original "${BK_ARMINO_PATH}/components/os_source/freertos_smp_v2p0/FreeRTOS-Kernel/tasks.c")
add_library(kernel STATIC "${original}" keep.c)
if(DUPLICATE)
  target_sources(kernel PRIVATE duplicate/tasks.c)
endif()
set_source_files_properties("${original}" TARGET_DIRECTORY kernel
  PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/tls.h;-DH2_KEEP=1")
function(armino_component_get_property output component property)
  if(NOT CONFIG_FREERTOS_SMP OR NOT component STREQUAL "os_source")
    message(FATAL_ERROR "Unexpected kernel request")
  endif()
  set(${output} kernel PARENT_SCOPE)
endfunction()
include("${H2_TRACE_COMPONENT}")
get_target_property(selected kernel SOURCES)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt" CONTENT "$<JOIN:$<TARGET_PROPERTY:kernel,SOURCES>,\\n>")
if(CONFIG_FREERTOS_SMP)
  get_source_file_property(options "${CMAKE_BINARY_DIR}/h2_bk_sdk_tasks.c"
    TARGET_DIRECTORY kernel COMPILE_OPTIONS)
  file(WRITE "${CMAKE_BINARY_DIR}/options.txt" "${options}")
endif()
''')
        for name, smp, duplicate in [("disabled", "OFF", "OFF"), ("enabled", "ON", "OFF"), ("duplicate", "ON", "ON")]:
            build = work / name
            run = subprocess.run([cmake, "-S", str(work), "-B", str(build),
                "-DBK_ARMINO_PATH=" + str(sdk / "ap"), "-DCONFIG_FREERTOS_SMP=" + smp,
                "-DDUPLICATE=" + duplicate, "-DH2_TRACE_COMPONENT=" + str(component)],
                capture_output=True, text=True, timeout=60)
            if duplicate == "ON":
                assert run.returncode != 0 and "Expected one BK SMP" in run.stderr
                continue
            assert run.returncode == 0, run.stdout + run.stderr
            selected = (build / "selected.txt").read_text().splitlines()
            assert len(selected) == 2 and "keep.c" in selected
            selected.remove("keep.c")
            if smp == "OFF":
                assert selected == [str(original)]
            else:
                generated = build / "h2_bk_sdk_tasks.c"
                assert selected == [str(generated)]
                assert generated.read_bytes().startswith(original.read_bytes())
                assert (build / "options.txt").read_text() == "-include;" + str(work / "tls.h") + ";-DH2_KEEP=1"
        changed = work / "changed" / relative
        changed.parent.mkdir(parents=True)
        changed.write_bytes(original.read_bytes() + b"\n/* changed kernel */\n")
        run = subprocess.run([cmake, "-S", str(work), "-B", str(work / "changed-build"),
            "-DBK_ARMINO_PATH=" + str(work / "changed"), "-DCONFIG_FREERTOS_SMP=ON",
            "-DH2_TRACE_COMPONENT=" + str(component)], capture_output=True, text=True, timeout=60)
        assert run.returncode != 0 and "Pinned BK kernel changed" in run.stderr
    assert hashlib.sha256(original.read_bytes()).hexdigest() == digest
    print(json.dumps(dict(result="PASS", sdk_commit=expected, original_sha256=digest,
        profiles=["enabled", "disabled", "duplicate rejected", "changed SDK rejected"],
        tls_cleanup_options_preserved=True)))


if __name__ == "__main__":
    main()
