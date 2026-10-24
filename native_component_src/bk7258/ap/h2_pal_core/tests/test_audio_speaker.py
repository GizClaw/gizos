"""Exercise real SDK speaker transitions and DMA ring code under delayed pause ACK."""
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
    end = source.index("{", start) + 1
    depth = 1
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
    source = sdk / "ap/components/audio_play/src/onboard_speaker_play.c"
    ring = sdk / "ap/middleware/driver/audio/audio_ring_buff.c"
    digests = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, ring)]
    component = repo / "native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_audio_speaker_component.cmake"
    probe = Path(__file__).with_name("audio_speaker_probe.c")
    compiler, cmake = shutil.which("cc"), shutil.which("cmake")
    assert compiler and cmake
    with tempfile.TemporaryDirectory(prefix="h2-bk-speaker-") as temporary:
        work = Path(temporary)
        (work / "keep.c").write_text("int keep(void) { return 0; }\n")
        (work / "duplicate").mkdir()
        (work / "duplicate/onboard_speaker_play.c").write_text("int duplicate(void) { return 0; }\n")
        (work / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.18)
project(speaker C)
set(original "${BK_ARMINO_PATH}/components/audio_play/src/onboard_speaker_play.c")
add_library(speaker STATIC "${original}" keep.c)
set_source_files_properties("${original}" TARGET_DIRECTORY speaker PROPERTIES COMPILE_OPTIONS "-DH2_KEEP=1")
if(DUPLICATE)
  target_sources(speaker PRIVATE duplicate/onboard_speaker_play.c)
endif()
function(armino_component_get_property output component property)
  if(NOT component STREQUAL "audio_play")
    message(FATAL_ERROR "unexpected component")
  endif()
  set(${output} speaker PARENT_SCOPE)
endfunction()
include("${H2_COMPONENT}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt" CONTENT "$<JOIN:$<TARGET_PROPERTY:speaker,SOURCES>,\\n>")
if(CONFIG_AUDIO_PLAY AND NOT DUPLICATE)
  get_source_file_property(options "${H2_BK_SPEAKER_OUTPUT}" TARGET_DIRECTORY speaker COMPILE_OPTIONS)
  file(WRITE "${CMAKE_BINARY_DIR}/options.txt" "${options}")
endif()
''')
        for name, enabled, duplicate in [("enabled", "ON", "OFF"), ("disabled", "OFF", "OFF"), ("duplicate", "ON", "ON")]:
            build = work / name
            result = subprocess.run([cmake, "-S", str(work), "-B", str(build),
                "-DBK_ARMINO_PATH=" + str(sdk / "ap"), "-DCONFIG_AUDIO_PLAY=" + enabled,
                "-DDUPLICATE=" + duplicate, "-DH2_COMPONENT=" + str(component)], capture_output=True, text=True, timeout=60)
            if duplicate == "ON":
                assert result.returncode != 0 and "Expected one SDK speaker source" in result.stderr
                continue
            assert result.returncode == 0, result.stdout + result.stderr
            selected = (build / "selected.txt").read_text().splitlines()
            assert len(selected) == 2 and "keep.c" in selected
            selected.remove("keep.c")
            assert selected == [str(build / "h2_bk_sdk_speaker_play.c") if enabled == "ON" else str(source)]
            if enabled == "ON":
                assert (build / "options.txt").read_text() == "-DH2_KEEP=1"
        includes = work / "include"
        includes.mkdir()
        (includes / "sdk.h").write_text('''#ifndef TEST_BK_SPEAKER_SDK_H
#define TEST_BK_SPEAKER_SDK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int bk_err_t;
#define BK_OK 0
#define BK_FAIL (-1)
#define BEKEN_WAIT_FOREVER UINT32_MAX
#endif
''')
        for header, body in {
            "common/bk_include.h": '#include "sdk.h"\n',
            "driver/dma.h": '#include "sdk.h"\ntypedef int dma_id_t;\n#define DMA_ID_MAX 16\nuint32_t bk_dma_get_enable_status(dma_id_t);\n',
        }.items():
            path = includes / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body)
        ring_functions = "\n".join(function(ring.read_text(), signature) for signature in [
            "void ring_buffer_clear(RingBufferContext* rb)",
            "uint32_t ring_buffer_write(RingBufferContext* rb, uint8_t* buffer, uint32_t size)"])
        for name, path in [("original", source), ("corrected", work / "enabled/h2_bk_sdk_speaker_play.c")]:
            output = work / name
            output.mkdir()
            text = path.read_text()
            functions = "\n".join(function(text, signature) for signature in [
                "static bk_err_t onboard_speaker_start(onboard_speaker_play_priv_t *onboard_spk)",
                "static bk_err_t onboard_speaker_stop(onboard_speaker_play_priv_t *onboard_spk)",
                "static int onboard_speaker_play_control(audio_play_t *play, audio_play_ctl_t ctl)"])
            worker = function(text, "static void spk_data_read_task_main(beken_thread_arg_t param_data)")
            idle = worker.split("case SPK_DATA_READ_IDLE:", 1)[1].split("break;", 1)[0]
            (output / "sdk_speaker_idle.inc").write_text(idle)
            for filename, code in [("sdk_speaker_functions.inc", functions), ("sdk_audio_ring.inc", ring_functions)]:
                (output / filename).write_text('#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"\n' + code + '\n#pragma GCC diagnostic pop\n')
            binary = output / "probe"
            result = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-I" + str(output), "-I" + str(includes), "-I" + str(source.parent.parent / "include"),
                "-I" + str(sdk / "ap/include"), "-include", str(includes / "sdk.h"), str(probe), "-o", str(binary)], capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            for profile in (["handoff", "bootstrap"] if name == "original" else ["handoff", "bootstrap", "errors"]):
                result = subprocess.run([str(binary), profile], capture_output=True, text=True, timeout=30)
                expected_code = (19 if profile == "handoff" else 20) if name == "original" else 0
                assert result.returncode == expected_code, (name, profile, result.stdout, result.stderr)
        for name, changed in [("speaker", source), ("ring", ring)]:
            altered = work / ("changed-" + name)
            altered.write_bytes(changed.read_bytes() + b"\n/* changed */\n")
            result = subprocess.run([cmake, "-DH2_BK_SPEAKER_INPUT=" + str(altered if name == "speaker" else source),
                "-DH2_BK_AUDIO_RING_INPUT=" + str(altered if name == "ring" else ring),
                "-DH2_BK_SPEAKER_OUTPUT=" + str(work / "rejected.c"), "-P", str(component.with_name("h2_bk_audio_speaker.cmake"))], capture_output=True, text=True, timeout=30)
            assert result.returncode != 0 and "revalidate restart ownership" in result.stderr
    assert [hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, ring)] == digests
    print(json.dumps(dict(result="PASS", sdk=expected, paused_offsets=18, original_controls_rejected=2,
        profiles=["in-flight writer", "read-pause bootstrap", "idempotent resume", "failed post/ack/source/start/DMA/prime", "source selection", "source options", "changed SDK rejected"])))


if __name__ == "__main__":
    main()
