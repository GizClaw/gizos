"""Bind an exact DMA/diagnostic source audit without changing device receipts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import check_qualification as qualification


def digest(value):
    return hashlib.sha256(value).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--validation-dir", type=Path, required=True)
    args = parser.parse_args()
    execution = json.loads((args.validation_dir / "dma-host-execution.json").read_text())
    log = (args.validation_dir / "display_config_host_test.raw.log").read_bytes()
    assert b"AMOLED_DMA_CONFIG_PASS default_rows=64 options=8,16,32,64 invalid_rows=0" in log
    assert execution["raw_log_sha256"] == digest(log)
    assert execution["executed_source_commit"] == qualification.AMOLED_DMA_EXECUTION_COMMIT
    assert execution["raw_log_sha256"] == qualification.AMOLED_DMA_LOG_SHA256
    assert execution["test_binary_sha256"] == qualification.AMOLED_DMA_BINARY_SHA256
    assert execution["source_sha256"] == qualification.AMOLED_DMA_INPUT_SHA256
    for path, expected in qualification.AMOLED_DMA_INPUT_SHA256.items():
        current = Path(path).read_bytes()
        executed = subprocess.check_output([
            "git", "show", execution["executed_source_commit"] + ":" + path])
        assert current == executed and digest(current) == expected, path
    value = {
        "schema": 1,
        "new_physical_run_claimed": False,
        "current_physical_qualification": False,
        "historical_qualification_sha256": digest(
            (qualification.ROOT / "qualification.json").read_bytes()),
        "qualified_driver_sha256": qualification.AMOLED_DMA_QUALIFIED_SHA256,
        "current_driver_sha256": digest(Path(qualification.AMOLED_DMA_SOURCE).read_bytes()),
        "closed_input_sha256": qualification.AMOLED_DMA_INPUT_SHA256,
        "host_validation": execution,
        "scope": "Exact opt-in DMA ceiling with unchanged default 0-to-64 behavior and diagnostics-only FT3168 delta. Historical physical/mobile/image/artifact receipts stay byte-identical. Current board source/artifact is not physically requalified; opt-in 8-row behavior is separate from old hardware acceptance. Native current-head builds and physical/finger checks remain separate evidence.",
    }
    (qualification.ROOT / "amoled_dma_maintenance.json").write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
