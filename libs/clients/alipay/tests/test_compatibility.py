"""Replay frozen public API inputs and expected outputs without an external SDK."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--case", required=True)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    if manifest["format"] != 1:
        raise ValueError("Unsupported compatibility fixture format")
    expected = manifest["cases"][args.case]
    compressed = args.fixture.read_bytes()
    if hashlib.sha256(compressed).hexdigest() != expected["compressed_sha256"]:
        raise ValueError("Compatibility fixture checksum mismatch")
    data = gzip.decompress(compressed)
    if hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise ValueError("Compatibility trace checksum mismatch")
    if len(data.splitlines()) != expected["records"]:
        raise ValueError("Compatibility trace is incomplete")
    with tempfile.TemporaryDirectory(
        prefix="alipay-compatibility-", dir=os.environ.get("TEST_TMPDIR")
    ) as directory:
        inputs = Path(directory) / "cases.jsonl"
        inputs.write_bytes(data)
        result = subprocess.run(
            [str(args.runner.resolve()), str(inputs)],
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        )
    actual = json.loads(result.stdout.strip().splitlines()[-1])
    if actual != expected["expected"]:
        raise AssertionError(
            json.dumps({"expected": expected["expected"], "actual": actual})
        )
    print(json.dumps({"case": args.case, "records": expected["records"], **actual}))


if __name__ == "__main__":
    main()
