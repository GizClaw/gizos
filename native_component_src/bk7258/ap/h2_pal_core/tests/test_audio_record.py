"""Exercise the repaired SDK record owner and close body without a source rewrite."""
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
    probe = Path(__file__).with_name("audio_record_probe.c")
    compiler = shutil.which("cc")
    assert compiler
    with tempfile.TemporaryDirectory(prefix="h2-bk-record-") as temporary:
        work = Path(temporary)
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
        for name, path, code in [("sdk", source, 0)]:
            backend = mic
            (includes / "sdk_mic_close.inc").write_text(function(backend.read_text(), "static int onboard_mic_record_close(audio_record_t *record)"))
            (includes / "sdk_mic_control.inc").write_text(function(backend.read_text(), "static int onboard_mic_record_control(audio_record_t *record, audio_record_ctl_t ctl)"))
            binary = work / name
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-I" + str(includes), "-I" + str(source.parent.parent / "include"), "-include", str(includes / "sdk.h"),
                str(path), str(probe), "-o", str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            assert result.returncode == code, result.stdout + result.stderr
            assert ("retained-context" if code else "PASS 128 cycles") in result.stdout
    assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
    print(json.dumps(dict(result="PASS", sdk=expected, cycles=128, fixed_sdk_validated=True,
        profiles=["NULL", "unopened", "closed", "allocation failure", "failed close retained", "retry"])))


if __name__ == "__main__":
    main()
