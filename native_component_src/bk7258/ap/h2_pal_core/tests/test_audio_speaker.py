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
    probe = Path(__file__).with_name("audio_speaker_probe.c")
    compiler = shutil.which("cc")
    assert compiler
    with tempfile.TemporaryDirectory(prefix="h2-bk-speaker-") as temporary:
        work = Path(temporary)
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
        for name, path in [("sdk", source)]:
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
            for profile in ["handoff", "bootstrap", "errors"]:
                result = subprocess.run([str(binary), profile], capture_output=True, text=True, timeout=30)
                expected_code = 0
                assert result.returncode == expected_code, (name, profile, result.stdout, result.stderr)
    assert [hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, ring)] == digests
    print(json.dumps(dict(result="PASS", sdk=expected, paused_offsets=18, fixed_sdk_validated=True,
        profiles=["in-flight writer", "read-pause bootstrap", "idempotent resume", "failed post/ack/source/start/DMA/prime"])))


if __name__ == "__main__":
    main()
