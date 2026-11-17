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
    parser.add_argument(
        "--exclude-transit-scripts",
        action="store_true",
        help="Exclude archived downloaded-script records; unsupported behavior is tested separately.",
    )
    parser.add_argument(
        "--exclude-image-decoding",
        action="store_true",
        help="Exclude archived image-decode comparison counts; the image API is unsupported.",
    )
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
    comparison_expected = dict(expected["expected"])
    excluded_images = 0
    if args.exclude_image_decoding:
        if args.case != "scan":
            raise ValueError("Image exclusions apply only to the scan fixture")
        excluded_images = comparison_expected["image_decode_comparisons"]
        if excluded_images <= 0:
            raise ValueError("Unexpected image-decode fixture scope")
        comparison_expected["image_decode_comparisons"] = 0
    excluded = 0
    if args.exclude_transit_scripts:
        if args.case != "transit":
            raise ValueError("Script exclusions apply only to the transit fixture")
        supported = []
        for raw in data.splitlines(keepends=True):
            record = json.loads(raw)
            if record.get("kind") == "transit" and record.get("entry") in {
                "native_script",
                "script_update",
            }:
                excluded += 1
            else:
                supported.append(raw)
        if not 0 < excluded < comparison_expected["public_transit_comparisons"]:
            raise ValueError("Unexpected downloaded-script fixture scope")
        comparison_expected["public_transit_comparisons"] -= excluded
        data = b"".join(supported)
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
    if actual != comparison_expected:
        raise AssertionError(
            json.dumps({"expected": comparison_expected, "actual": actual})
        )
    print(
        json.dumps(
            {
                "case": args.case,
                "records": expected["records"],
                "compared_records": expected["records"] - excluded,
                "excluded_transit_script_records": excluded,
                "excluded_image_decode_comparisons": excluded_images,
                **actual,
            }
        )
    )


if __name__ == "__main__":
    main()
