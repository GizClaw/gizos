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
    cc = shutil.which("cc")
    assert cc, "a native C compiler is required"
    with tempfile.TemporaryDirectory(prefix="h2-spi-completion-") as temporary:
        work = Path(temporary)
        baseline = work / "baseline_spi.c"
        baseline.write_text(subprocess.check_output(["git", "-C", str(sdk), "show", "9020466789460701a62a823deb4a98edd6ea0e5d:ap/middleware/driver/spi/spi_driver.c"], text=True))
        generated = original
        corrected_profiles, negative_controls = 0, 0
        for name, source in (("original", baseline), ("corrected", generated)):
            unit, binary = work / (name + ".c"), work / name
            functions = "\n".join(function(source.read_text(), symbol) for symbol in (
                "bk_spi_clr_tx", "bk_spi_clr_rx", "spi_isr_common",
                "spi_dma_tx_finish_handler", "spi_dma_rx_finish_handler",
                "bk_spi_write_bytes", "bk_spi_read_bytes",
                "bk_spi_dma_write_bytes", "bk_spi_dma_read_bytes", "bk_spi_dma_duplex_xfer"))
            unit.write_text(probe.replace("/* SDK_FUNCTIONS */", functions))
            # The SDK uses 32-bit DMA addresses; the host HAL never dereferences them.
            command = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-pointer-to-int-cast", "-pthread", str(unit), "-o", str(binary)]
            if args.sanitize and name == "corrected":
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=60)
            for direction in ("tx", "rx"):
                for first in ("dma", "cpu"):
                    for next_mode in ("dma", "cpu"):
                        for role in ("master", "slave"):
                            for irq in ([None, "ordinary"] if next_mode == "dma" else [None]):
                                command = [str(binary), direction, first, next_mode, role]
                                if irq:
                                    command.append(irq)
                                run = subprocess.run(command, capture_output=True, text=True, timeout=15)
                                if name == "original":
                                    assert run.returncode != 0, "uncorrected SDK unexpectedly passed"
                                    negative_controls += 1
                                    expected_errors = ["second SPI completion lost"]
                                    if irq:
                                        expected_errors.append("ordinary SPI IRQ consumed DMA completion")
                                    assert any(error in run.stderr for error in expected_errors), run.stderr
                                else:
                                    assert run.returncode == 0, run.stdout + run.stderr
                                    corrected_profiles += sum(line.startswith("PASS ") for line in run.stdout.splitlines())
                                    print(run.stdout, end="")
            for role in ("master", "slave"):
                for retry in ("normal", "retry"):
                    run = subprocess.run([str(binary), "duplex", role, retry],
                                         capture_output=True, text=True, timeout=15)
                    if name == "original":
                        assert run.returncode != 0, "uncorrected duplex SDK unexpectedly passed"
                        negative_controls += 1
                        assert "ordinary SPI IRQ consumed duplex DMA completion" in run.stderr, run.stderr
                    else:
                        assert run.returncode == 0, run.stdout + run.stderr
                        corrected_profiles += sum(line.startswith("PASS ") for line in run.stdout.splitlines())
                        print(run.stdout, end="")
    assert hashlib.sha256(original.read_bytes()).hexdigest() == before, "sealed SDK was modified"
    print(json.dumps(dict(result="PASS", sdk_commit=actual, sdk_source_sha256=before,
                          original_negative_controls="TX/RX FAIL as expected",
                          corrected="TX/RX CPU/DMA SMP handoff, ordinary IRQ isolation and duplex retry PASS",
                          corrected_profiles=corrected_profiles, negative_controls=negative_controls,
                          sanitized=args.sanitize),
                     sort_keys=True))


if __name__ == "__main__":
    main()
