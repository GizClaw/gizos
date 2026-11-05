"""Synthetic native CP/AP and Bootloader images for offline encryption tests."""

import argparse
import json
from pathlib import Path
import struct
import zlib

from tools.bazel.bk7258_flash_aes import flash_crc


def fixture(image):
    full = image == "full"
    base = 0 if full else 0x240000
    cp_offset = 4096 if full else 0
    ap_offset = cp_offset + 4096
    plain = bytearray(b"\xff" * (16384 if full else 12288))
    for offset in ((0, cp_offset, ap_offset) if full else (0, ap_offset)):
        struct.pack_into("<II", plain, offset, 0x28030000, 0x02000000 + base + offset + 0x81)
    if full:
        plain[256:272] = b"BK7236-test-only"
    payload = plain[cp_offset:cp_offset + 8192]
    fnv = 0x811C9DC5
    for byte in payload:
        fnv = ((fnv ^ byte) * 0x1000193) & 0xFFFFFFFF
    header = bytearray(96)
    header[:4] = b"RBL\0"
    struct.pack_into("<4I", header, 76, zlib.crc32(payload), fnv, 8192, 8192)
    struct.pack_into("<I", header, 92, zlib.crc32(header[:92]))
    plain[-4096:-4000] = header
    start = base // 32 * 34
    sections = ([dict(Name="primary_bootloader", Offset=0, Size=4352, Execute=True)] if full else []) + [
        dict(Name="primary_cp_app", Offset=start + cp_offset // 32 * 34, Size=4352, Execute=True),
        dict(Name="primary_ap_app", Offset=start + ap_offset // 32 * 34, Size=8704, Execute=True),
    ]
    metadata = dict(crc_enable=True, section=sections)
    source = b"".join(plain[i:i + 32] + flash_crc(plain[i:i + 32]) for i in range(0, len(plain), 32))
    return source, metadata


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, choices=("app", "full"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    args = parser.parse_args()
    source, metadata = fixture(args.image)
    args.source.parent.mkdir(parents=True, exist_ok=True)
    args.source.write_bytes(source)
    args.metadata.mkdir(parents=True, exist_ok=True)
    (args.metadata / "partitions.json").write_text(json.dumps(metadata))


if __name__ == "__main__":
    main()
