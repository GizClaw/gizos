"""Pack immutable ESP-SR model files using their documented LE index format."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


def fixed_name(name: str) -> bytes:
    encoded = name.encode("ascii")
    if not encoded or len(encoded) >= 32 or "/" in name or "\\" in name:
        raise ValueError("model and file names must be bounded ASCII basenames")
    return encoded.ljust(32, b"\0")


def pack(model_name: str, files: dict[str, bytes]) -> bytes:
    if not files or len(files) > 32 or "_MODEL_INFO_" not in files:
        raise ValueError("model requires metadata and at most 32 files")
    header_size = 4 + 36 + len(files) * 40
    header = struct.pack("<I", 1) + fixed_name(model_name) + struct.pack("<I", len(files))
    payload = bytearray()
    for name, data in sorted(files.items()):
        if not data or data.startswith(b"version https://git-lfs.github.com/spec/"):
            raise ValueError("empty model data or unresolved Git LFS pointer")
        # SIMD readers receive aligned views even when upstream file sizes are odd.
        padding = (-(header_size + len(payload))) % 16
        payload.extend(b"\0" * padding)
        offset = header_size + len(payload)
        if offset + len(data) > 1024 * 1024:
            raise ValueError("model exceeds the one-MiB loader budget")
        header += fixed_name(name) + struct.pack("<II", offset, len(data))
        payload.extend(data)
    return header + payload


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-name", required=True)
    parser.add_argument("--info", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_bytes(pack(args.model_name, {
        "_MODEL_INFO_": args.info.read_bytes(),
        "wn9_index": args.index.read_bytes(),
        "wn9_data": args.data.read_bytes(),
    }))


if __name__ == "__main__":
    main()
