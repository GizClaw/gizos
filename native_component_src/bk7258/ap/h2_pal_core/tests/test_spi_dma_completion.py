"""Run real pinned SDK transfers and completion ISRs across a forced SMP cut."""
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
    match = re.search(
        r"^(?:static\s+)?(?:void|bk_err_t)\s+" + name + r"\s*\([^;{]*\)\s*\{",
        source, re.M)
    assert match, "missing pinned SDK function: " + name
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
    original = sdk / "ap/middleware/driver/spi/spi_driver.c"
    before = hashlib.sha256(original.read_bytes()).hexdigest()
    probe = (component / "tests/spi_dma_completion_probe.c").read_text()
    cc, cmake = shutil.which("cc"), shutil.which("cmake")
    assert cc and cmake, "a native C compiler and CMake are required"
    with tempfile.TemporaryDirectory(prefix="h2-spi-completion-") as temporary:
        work = Path(temporary)
        (work / "keep.c").write_text("int unrelated_driver(void) { return 0; }\n")
        (work / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
project(spi_completion C)
add_library(driver STATIC "${BK_ARMINO_PATH}/middleware/driver/spi/spi_driver.c" keep.c)
function(armino_component_get_property output component property)
  if(NOT CONFIG_SPI OR NOT component STREQUAL "driver")
    message(FATAL_ERROR "Disabled SPI requested the driver")
  endif()
  set(${output} driver PARENT_SCOPE)
endfunction()
include("${H2_SPI_COMPONENT}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt"
  CONTENT "$<JOIN:$<TARGET_PROPERTY:driver,SOURCES>,\\n>")
''')
        generated = None
        for spi, dma in (("OFF", "OFF"), ("ON", "OFF"), ("OFF", "ON"), ("ON", "ON")):
            build = work / (spi + "-" + dma)
            subprocess.run([cmake, "-S", str(work), "-B", str(build),
                            "-DBK_ARMINO_PATH=" + str(sdk / "ap"),
                            "-DCONFIG_SPI=" + spi, "-DCONFIG_SPI_DMA=" + dma,
                            "-DH2_SPI_COMPONENT=" + str(component / "src/h2_bk_spi_dma_component.cmake")],
                           check=True, capture_output=True, text=True)
            selected = (build / "selected.txt").read_text().splitlines()
            assert len(selected) == 2 and "keep.c" in selected, selected
            selected.remove("keep.c")
            if spi == "ON":
                generated = Path(selected[0])
                assert generated.parent == build and generated.is_file()
            else:
                assert Path(selected[0]) == original, selected
        assert generated is not None
        changed = work / "changed.c"
        changed.write_text(original.read_text() + "\n/* changed SDK */\n")
        for name, source in (("changed", changed), ("repeated", generated)):
            output = work / (name + "-output.c")
            run = subprocess.run(
                [cmake, "-DH2_BK_SPI_INPUT=" + str(source),
                 "-DH2_BK_SPI_OUTPUT=" + str(output), "-P",
                 str(component / "src/h2_bk_spi_dma_completion.cmake")],
                capture_output=True, text=True, check=False)
            assert run.returncode != 0 and not output.exists(), name
        for name, source in (("original", original), ("corrected", generated)):
            unit, binary = work / (name + ".c"), work / name
            functions = "\n".join(function(source.read_text(), symbol) for symbol in (
                "bk_spi_clr_tx", "bk_spi_clr_rx", "spi_isr_common",
                "spi_dma_tx_finish_handler", "spi_dma_rx_finish_handler",
                "bk_spi_write_bytes", "bk_spi_read_bytes",
                "bk_spi_dma_write_bytes", "bk_spi_dma_read_bytes"))
            unit.write_text(probe.replace("/* SDK_FUNCTIONS */", functions))
            command = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-pthread", str(unit), "-o", str(binary)]
            if args.sanitize and name == "corrected":
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=60)
            for direction in ("tx", "rx"):
                for first in ("dma", "cpu"):
                    for next_mode in ("dma", "cpu"):
                        for role in ("master", "slave"):
                            run = subprocess.run(
                                [str(binary), direction, first, next_mode, role],
                                capture_output=True, text=True, timeout=15)
                            if name == "original":
                                assert run.returncode != 0, "uncorrected SDK unexpectedly passed"
                                assert "second SPI completion lost" in run.stderr, run.stderr
                            else:
                                assert run.returncode == 0, run.stdout + run.stderr
                                print(run.stdout, end="")
    assert hashlib.sha256(original.read_bytes()).hexdigest() == before, "sealed SDK was modified"
    print(json.dumps(dict(result="PASS", sdk_commit=actual, sdk_source_sha256=before,
                          original_negative_controls="TX/RX FAIL as expected",
                          corrected="TX/RX CPU/DMA master/slave port pairs PASS",
                          sanitized=args.sanitize),
                     sort_keys=True))


if __name__ == "__main__":
    main()
