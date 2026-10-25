"""Execute the pinned SDK record owner and close body, including a negative control."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


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
    source = sdk / "ap/components/audio_record/src/audio_record.c"
    mic = source.with_name("onboard_mic_record.c")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    component = repo / "native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_audio_record_component.cmake"
    probe = Path(__file__).with_name("audio_record_probe.c")
    compiler, cmake = shutil.which("cc"), shutil.which("cmake")
    assert compiler and cmake
    with tempfile.TemporaryDirectory(prefix="h2-bk-record-") as temporary:
        work = Path(temporary)
        (work / "keep.c").write_text("int keep(void) { return 0; }\n")
        (work / "duplicate").mkdir()
        (work / "duplicate/audio_record.c").write_text("int duplicate(void) { return 0; }\n")
        (work / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.18)
project(audio_record C)
set(original "${BK_ARMINO_PATH}/components/audio_record/src/audio_record.c")
set(mic "${BK_ARMINO_PATH}/components/audio_record/src/onboard_mic_record.c")
add_library(record STATIC "${original}" "${mic}" keep.c)
set_source_files_properties("${original}" TARGET_DIRECTORY record PROPERTIES COMPILE_OPTIONS "-DH2_PRESERVED=1")
if(DUPLICATE)
  target_sources(record PRIVATE duplicate/audio_record.c)
endif()
function(armino_component_get_property output component property)
  if(NOT component STREQUAL "audio_record")
    message(FATAL_ERROR "unexpected component")
  endif()
  set(${output} record PARENT_SCOPE)
endfunction()
include("${H2_COMPONENT}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt" CONTENT "$<JOIN:$<TARGET_PROPERTY:record,SOURCES>,\\n>")
if(CONFIG_AUDIO_RECORD AND NOT DUPLICATE)
  get_source_file_property(options "${H2_BK_AUDIO_RECORD_OUTPUT}" TARGET_DIRECTORY record COMPILE_OPTIONS)
  file(WRITE "${CMAKE_BINARY_DIR}/options.txt" "${options}")
endif()
''')
        for name, enabled, duplicate in [("enabled", "ON", "OFF"), ("disabled", "OFF", "OFF"), ("duplicate", "ON", "ON")]:
            build = work / name
            result = subprocess.run([cmake, "-S", str(work), "-B", str(build),
                "-DBK_ARMINO_PATH=" + str(sdk / "ap"), "-DCONFIG_AUDIO_RECORD=" + enabled,
                "-DDUPLICATE=" + duplicate, "-DH2_COMPONENT=" + str(component)], capture_output=True, text=True, timeout=60)
            if duplicate == "ON":
                assert result.returncode != 0 and "Expected one SDK audio record source" in result.stderr
                continue
            assert result.returncode == 0, result.stdout + result.stderr
            selected = (build / "selected.txt").read_text().splitlines()
            assert len(selected) == 3 and "keep.c" in selected
            selected.remove("keep.c")
            assert set(selected) == ({str(build / "h2_bk_sdk_audio_record.c"), str(build / "h2_bk_sdk_mic_record.c")} if enabled == "ON" else {str(source), str(mic)})
            if enabled == "ON":
                assert (build / "options.txt").read_text() == "-DH2_PRESERVED=1"
        includes = work / "include"
        includes.mkdir()
        (includes / "sdk.h").write_text('''#ifndef TEST_BK_RECORD_SDK_H
#define TEST_BK_RECORD_SDK_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int bk_err_t;
#define BK_OK 0
#define BK_FAIL (-1)
#define BK_LOGI(...) ((void)0)
#define BK_LOGW(...) ((void)0)
#define BK_LOGE(...) ((void)0)
#define BK_LOGD(...) ((void)0)
void *os_malloc(size_t);
void os_free(void *);
#define os_memset memset
#define os_memcpy memcpy
#endif
''')
        for header in ["common/bk_include.h", "os/os.h", "os/mem.h", "os/str.h"]:
            path = includes / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "sdk.h"\n')
        for name, path, code in [("original", source, 19), ("corrected", work / "enabled/h2_bk_sdk_audio_record.c", 0)]:
            backend = mic if name == "original" else work / "enabled/h2_bk_sdk_mic_record.c"
            (includes / "sdk_mic_close.inc").write_text(function(backend.read_text(), "static int onboard_mic_record_close(audio_record_t *record)"))
            (includes / "sdk_mic_control.inc").write_text(function(backend.read_text(), "static int onboard_mic_record_control(audio_record_t *record, audio_record_ctl_t ctl)"))
            binary = work / name
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-I" + str(includes), "-I" + str(source.parent.parent / "include"), "-include", str(includes / "sdk.h"),
                str(path), str(probe), "-o", str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            assert result.returncode == code, result.stdout + result.stderr
            assert ("retained-context" if code else "PASS 128 cycles") in result.stdout
        dma_profiles = {}
        for name, backend in [("original", mic), ("corrected", work / "enabled/h2_bk_sdk_mic_record.c")]:
            (includes / "sdk_mic_dma_config.inc").write_text(function(
                backend.read_text(), "static bk_err_t aud_adc_dma_config(onboard_mic_record_priv_t *onboard_mic)"))
            binary = work / (name + "-dma")
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-pointer-to-int-cast", "-fsanitize=address,undefined", "-I" + str(includes),
                str(probe.with_name("audio_record_dma_probe.c")), "-o", str(binary)],
                check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, result.stdout + result.stderr
            dma_profiles[name] = [json.loads(line) for line in result.stdout.splitlines()]
        assert len(dma_profiles["original"]) == len(dma_profiles["corrected"]) == 6
        for original, corrected in zip(dma_profiles["original"], dma_profiles["corrected"]):
            assert original["destination_width"] == 2
            expected_profile = dict(original, destination_width=original["source_width"])
            assert corrected == expected_profile
            if corrected["channels"] == 2:
                assert corrected == original
        for name, changed in [("record", source), ("backend", mic)]:
            altered = work / ("changed-" + name)
            altered.write_bytes(changed.read_bytes() + b"\n/* changed */\n")
            result = subprocess.run([cmake, "-DH2_BK_AUDIO_RECORD_INPUT=" + str(altered if name == "record" else source),
                "-DH2_BK_AUDIO_MIC_INPUT=" + str(altered if name == "backend" else mic),
                "-DH2_BK_AUDIO_RECORD_OUTPUT=" + str(work / "rejected.c"), "-P", str(component.with_name("h2_bk_audio_record.cmake"))], capture_output=True, text=True, timeout=30)
            assert result.returncode != 0 and "revalidate context ownership" in result.stderr
    assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
    print(json.dumps(dict(result="PASS", sdk=expected, cycles=128, original_rejected=True,
        profiles=["NULL", "unopened", "closed", "allocation failure", "failed close retained", "retry", "disabled", "duplicate source", "changed SDK rejected", "source options preserved", "mono ADC beat width", "stereo descriptor unchanged", "DMA frame/ring bounds", "DMA setup failure cleanup"])))


if __name__ == "__main__":
    main()
