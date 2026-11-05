"""Write synthetic BK7258 native outputs; never hardware images."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[5]))
from tools.bazel.bk7258_flash_aes import flash_crc

# Logical pages: bootloader, CP, AP, RBL. A managed image omits the bootloader.
PAGE = 4096
PHYSICAL_PAGE = 4352
LOADER_PHYSICAL_BASE = PHYSICAL_PAGE
APP_PHYSICAL_BASE = 0x264000


def _rbl_header(payload: bytes) -> bytes:
    fnv = 0x811C9DC5
    for byte in payload:
        fnv = ((fnv ^ byte) * 0x1000193) & 0xFFFFFFFF
    header = bytearray(96)
    header[:4] = b"RBL\0"
    struct.pack_into("<4I", header, 76, zlib.crc32(payload), fnv, len(payload), len(payload))
    struct.pack_into("<I", header, 92, zlib.crc32(header[:92]))
    return bytes(header)


def _stored(plain: bytes) -> bytes:
    return b"".join(plain[i:i + 32] + flash_crc(plain[i:i + 32]) for i in range(0, len(plain), 32))


def _managed_plain(physical_base: int) -> bytes:
    logical_base = physical_base // 34 * 32
    plain = bytearray(b"\xff" * (3 * PAGE))
    for offset in (0, PAGE):
        struct.pack_into("<II", plain, offset, 0x28030000, 0x02000000 + logical_base + offset + 0x81)
    plain[2 * PAGE:2 * PAGE + 96] = _rbl_header(bytes(plain[:2 * PAGE]))
    return bytes(plain)


def partition_table(physical_base: int) -> dict:
    sections = [
        {"Name": "primary_cp_app", "Offset": physical_base, "Size": PHYSICAL_PAGE, "Execute": True},
        {"Name": "primary_ap_app", "Offset": physical_base + PHYSICAL_PAGE, "Size": 2 * PHYSICAL_PAGE, "Execute": True},
    ]
    if physical_base == LOADER_PHYSICAL_BASE:
        sections.insert(0, {"Name": "primary_bootloader", "Offset": 0, "Size": PHYSICAL_PAGE, "Execute": True})
    return {"crc_enable": True, "section": sections}


def managed_image(physical_base: int) -> bytes:
    return _stored(_managed_plain(physical_base))


def recovery_image() -> bytes:
    """Bootloader page followed by the Loader's managed image."""
    bootloader = bytearray(b"\xff" * PAGE)
    struct.pack_into("<II", bootloader, 0, 0x28030000, 0x02000081)
    bootloader[256:272] = b"BK7236-test-only"
    return _stored(bytes(bootloader) + _managed_plain(LOADER_PHYSICAL_BASE))


def write_outputs(role: str, managed: Path, recovery: Path, metadata: Path, config: Path) -> None:
    physical_base = LOADER_PHYSICAL_BASE if role == "h2loader" else APP_PHYSICAL_BASE
    managed.write_bytes(managed_image(physical_base))
    recovery.write_bytes(recovery_image())
    metadata.mkdir(parents=True, exist_ok=True)
    (metadata / "partitions.json").write_text(json.dumps(partition_table(physical_base)), encoding="utf-8")
    config.write_text(json.dumps({"offset": "0x00000000", "baud": 115200}), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("app", "h2loader"), required=True)
    for name in ("managed", "recovery", "metadata", "config"):
        parser.add_argument("--" + name, type=Path, required=True)
    arguments = parser.parse_args()
    write_outputs(arguments.role, arguments.managed, arguments.recovery, arguments.metadata, arguments.config)


if __name__ == "__main__":
    main()
