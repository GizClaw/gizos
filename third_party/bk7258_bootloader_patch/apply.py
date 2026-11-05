#!/usr/bin/env python3
"""Apply a native-built mapped_ab_v1 payload to an unencrypted native image.

This tool has no key, device access, SDK mutation, compiler discovery or GizOS
imports. Compilation and image ownership stay in the native CMake invocation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

MAGIC = 0x42414D48
VERSION = 1
FLAGS = 7
DESCRIPTOR_OFFSET = 0x7F00
PAYLOAD_OFFSET = 0x8000
ENTRY_OFFSET = 0x2070
LIMIT = 16 * 1024 * 1024
ALIGNMENT = 68 * 1024
SOURCE_SIZE = 18720
SOURCE_SHA256 = "9ce0da920a751ea8b635f363b898625e9da6d8355c10c18f2f18466bf8b66c41"
PAYLOAD_SHA256 = "3adfdca22717d074ad70a0fe419003be0aa1b7e7954a2e7af82789e7864a18f3"
# The SDK appends its partition table to the pinned bootloader bytes.
TABLE_OFFSET = SOURCE_SIZE
TABLE_MAGIC = b"01PE"
TABLE_RECORD_SIZE = 64
XIP_BASE = 0x02000000
RBL_PAGE_SIZE = 4096


class PatchError(ValueError):
    """Unsupported or inconsistent bootloader input."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def flash_crc(data: bytes) -> bytes:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x8005 if crc & 0x8000 else 0)) & 0xFFFF
    return crc.to_bytes(2, "big")


def decode_storage(image: bytes) -> bytes:
    if not image or len(image) > LIMIT or len(image) % 34:
        raise PatchError("native image length is invalid")
    blocks = []
    for offset in range(0, len(image), 34):
        block = image[offset:offset + 34]
        if block != b"\xff" * 34 and flash_crc(block[:32]) != block[32:]:
            raise PatchError("native image storage CRC is invalid")
        blocks.append(block[:32])
    return b"".join(blocks)


def encode_storage(plain: bytes) -> bytes:
    if len(plain) % 32:
        raise PatchError("logical image is not CRC-unit aligned")
    return b"".join(plain[i:i + 32] + flash_crc(plain[i:i + 32])
                    for i in range(0, len(plain), 32))


def load_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    required = {
        "schema": 1, "contract": "mapped_ab_v1", "source_size": SOURCE_SIZE,
        "source_sha256": SOURCE_SHA256, "payload_sha256": PAYLOAD_SHA256,
        "entry_offset": ENTRY_OFFSET,
        "entry_original_hex": "10b598b0", "descriptor_offset": DESCRIPTOR_OFFSET,
        "payload_offset": PAYLOAD_OFFSET, "maximum_payload_size": 0x8000,
        "vendor_lookup_thumb_address": 0x02002D85,
        "physical_flash_limit": LIMIT, "logging_changes": False,
    }
    if type(manifest) is not dict or manifest != required:
        raise PatchError("unsupported bootloader patch manifest")
    return manifest


def layout_from_metadata(metadata: dict, image_size: int) -> dict[str, int]:
    if type(metadata) is not dict or metadata.get("crc_enable") is not True:
        raise PatchError("mapped AB requires CRC-enabled partition metadata")
    sections = metadata.get("section")
    if not isinstance(sections, list) or any(type(p) is not dict for p in sections):
        raise PatchError("invalid partition list")
    by_name = {}
    ranges = []
    for part in sections:
        name, offset, size = part.get("Name"), part.get("Offset"), part.get("Size")
        if not isinstance(name, str) or name in by_name or type(offset) is not int or type(size) is not int:
            raise PatchError("invalid or duplicate partition")
        if offset < 0 or size <= 0 or offset >= LIMIT or size > LIMIT - offset:
            raise PatchError("partition exceeds flash bounds")
        by_name[name] = part
        ranges.append((offset, offset + size))
    ranges.sort()
    if any(a[1] > b[0] for a, b in zip(ranges, ranges[1:])):
        raise PatchError("partitions overlap")
    try:
        boot, cp, ap, trial = [by_name[name] for name in (
            "primary_bootloader", "primary_cp_app", "primary_ap_app", "s_app")]
    except KeyError:
        raise PatchError("mapped AB requires bootloader, CP, AP and trial partitions") from None
    if (boot["Offset"] != 0 or cp["Offset"] != boot["Size"] or
        ap["Offset"] != cp["Offset"] + cp["Size"] or
        image_size != ap["Offset"] + ap["Size"] or
        any(p.get("Execute") is not True for p in (boot, cp, ap)) or
        trial.get("Execute") is not False):
        raise PatchError("unsupported contiguous Loader native image")
    values = dict(boot_size=boot["Size"], loader_offset=cp["Offset"],
                  loader_size=cp["Size"] + ap["Size"], trial_offset=trial["Offset"],
                  trial_size=trial["Size"], cp_size=cp["Size"])
    if any(value % ALIGNMENT for value in values.values()):
        raise PatchError("mapped AB geometry must be erase/CRC/page aligned")
    if (values["loader_size"] > values["trial_size"] or
        values["loader_offset"] + values["loader_size"] > values["trial_offset"]):
        raise PatchError("insufficient or unsupported mapped AB windows")
    return values


def validate_partition_table(plain: bytes, geometry: dict[str, int]) -> None:
    """The checker compares these records with the descriptor on every boot."""
    records = {}
    for offset in range(TABLE_OFFSET, len(plain) - TABLE_RECORD_SIZE, TABLE_RECORD_SIZE):
        record = plain[offset:offset + TABLE_RECORD_SIZE]
        if record[:4] != TABLE_MAGIC:
            break
        name = record[4:36].split(b"\0", 1)[0]
        records.setdefault(name, []).append(struct.unpack_from("<II", record, 52))
    expected = {b"appa": (geometry["loader_offset"], geometry["loader_size"]),
                b"s_app": (geometry["trial_offset"], geometry["trial_size"])}
    if any(records.get(name) != [value] for name, value in expected.items()):
        raise PatchError("vendor partition table does not match the mapped AB geometry")


def validate_loader_image(image: bytes, plain: bytes, geometry: dict[str, int]) -> None:
    """Apply the checker's A rules, so an unbootable pairing fails at build time."""
    start = geometry["loader_offset"] // 34 * 32
    size = geometry["loader_size"] // 34 * 32
    cp_size = geometry["cp_size"] // 34 * 32
    view = plain[start:start + size]
    base = XIP_BASE + start
    head = view[size - RBL_PAGE_SIZE:size - RBL_PAGE_SIZE + 96]
    if len(view) != size or len(head) != 96:
        raise PatchError("native image does not contain the Loader window")
    payload_crc, payload_hash, stored_size, raw_size, head_crc = struct.unpack_from("<5I", head, 76)
    if (head[:4] != b"RBL\0" or head[4:6] != b"\0\0" or zlib.crc32(head[:92]) != head_crc or
        stored_size != raw_size or not stored_size or stored_size % 32 or
        stored_size < cp_size + 8 or stored_size > size - RBL_PAGE_SIZE):
        raise PatchError("Loader RBL header fails the mapped AB checker")
    for offset, end in ((0, cp_size), (cp_size, stored_size)):
        sp, pc = struct.unpack_from("<II", view, offset)
        if (not 0x28000000 <= sp <= 0x280A0000 or sp & 7 or not pc & 1 or
            not base + offset <= pc & ~1 < base + end):
            raise PatchError("Loader vectors fail the mapped AB checker")
    # An erased unit decodes as 0xFF but raises the controller's CRC error count.
    stored = geometry["loader_offset"]
    units = [*range(stored, stored + stored_size // 32 * 34, 34),
             *(stored + (size - RBL_PAGE_SIZE + offset) // 32 * 34
               for offset in (0, 32, 64))]
    if any(image[unit:unit + 34] == b"\xff" * 34 for unit in units):
        raise PatchError("Loader contains erased units the checker reads")
    digest = 0x811C9DC5
    for byte in view[:stored_size]:
        digest = ((digest ^ byte) * 0x1000193) & 0xFFFFFFFF
    if digest != payload_hash or zlib.crc32(view[:stored_size]) != payload_crc:
        raise PatchError("Loader digests fail the mapped AB checker")


def validate_branch(branch: bytes) -> None:
    if len(branch) != 4:
        raise PatchError("checker entry must be exactly four bytes")
    first, second = struct.unpack("<HH", branch)
    if first & 0xF800 != 0xF000 or second & 0xD000 != 0x9000:
        raise PatchError("checker entry is not Thumb B.W")
    sign = (first >> 10) & 1
    i1 = 1 ^ ((second >> 13) & 1) ^ sign
    i2 = 1 ^ ((second >> 11) & 1) ^ sign
    immediate = sign << 24 | i1 << 23 | i2 << 22 | (first & 0x3FF) << 12 | (second & 0x7FF) << 1
    if sign:
        immediate -= 1 << 25
    if ENTRY_OFFSET + 4 + immediate != PAYLOAD_OFFSET:
        raise PatchError("checker entry branches outside the payload entry")


def apply_patch(image: bytes, metadata: dict, payload: bytes, branch: bytes,
                manifest: dict) -> tuple[bytes, dict]:
    geometry = layout_from_metadata(metadata, len(image))
    plain = decode_storage(image)
    if len(plain) < SOURCE_SIZE or sha256(plain[:SOURCE_SIZE]) != manifest["source_sha256"]:
        raise PatchError("native image does not contain the pinned vendor bootloader")
    original = bytes.fromhex(manifest["entry_original_hex"])
    if plain[ENTRY_OFFSET:ENTRY_OFFSET + len(original)] != original:
        raise PatchError("original checker instructions do not match")
    validate_partition_table(plain, geometry)
    validate_loader_image(image, plain, geometry)
    validate_branch(branch)
    boot_logical = geometry["boot_size"] // 34 * 32
    if not payload or len(payload) > manifest["maximum_payload_size"] or len(payload) > boot_logical - PAYLOAD_OFFSET:
        raise PatchError("payload exceeds the bootloader window")
    if (plain[DESCRIPTOR_OFFSET:DESCRIPTOR_OFFSET + 64] != b"\xff" * 64 or
        plain[PAYLOAD_OFFSET:PAYLOAD_OFFSET + len(payload)] != b"\xff" * len(payload)):
        raise PatchError("patch destination is not unused vendor padding")
    if sha256(payload) != manifest["payload_sha256"]:
        raise PatchError("payload differs from the qualified contract")
    fields = [MAGIC, VERSION, 64, FLAGS, geometry["boot_size"], geometry["loader_offset"],
              geometry["loader_size"], geometry["trial_offset"], geometry["trial_size"],
              geometry["cp_size"], PAYLOAD_OFFSET, len(payload), zlib.crc32(payload), ENTRY_OFFSET, 0]
    descriptor = struct.pack("<15I", *fields)
    descriptor += struct.pack("<I", zlib.crc32(descriptor))
    patched = bytearray(plain)
    patched[ENTRY_OFFSET:ENTRY_OFFSET + 4] = branch
    patched[DESCRIPTOR_OFFSET:DESCRIPTOR_OFFSET + 64] = descriptor
    patched[PAYLOAD_OFFSET:PAYLOAD_OFFSET + len(payload)] = payload
    # Native assembly and the BootROM marker/log gate remain SDK-owned.
    if (patched[boot_logical:] != plain[boot_logical:] or
        patched[0x100:0x110] != plain[0x100:0x110] or
        patched[0x31C:0x320] != plain[0x31C:0x320]):
        raise PatchError("patch changed an immutable native region")
    stored = bytearray(image)
    for unit in range(0, boot_logical, 32):
        if patched[unit:unit + 32] != plain[unit:unit + 32]:
            at = unit // 32 * 34
            stored[at:at + 34] = encode_storage(bytes(patched[unit:unit + 32]))
    result = bytes(stored)
    receipt = {
        "schema": 1, "contract": "mapped_ab_v1", "contract_version": VERSION,
        "source_bootloader_sha256": manifest["source_sha256"],
        "logical_bootloader_sha256": sha256(bytes(patched[:boot_logical])),
        "stored_bootloader_sha256": sha256(result[:geometry["boot_size"]]),
        "native_image_sha256": sha256(result), "payload_sha256": sha256(payload),
        "descriptor_crc32": zlib.crc32(descriptor[:60]),
        "layout": geometry, "forced_uart_logging": False,
    }
    return result, receipt


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("manifest", "image", "metadata", "payload", "branch", "output", "receipt"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        if not 0 < args.image.stat().st_size <= LIMIT:
            raise PatchError("native image exceeds bounds")
        if not 0 < args.payload.stat().st_size <= 0x8000 or args.branch.stat().st_size != 4:
            raise PatchError("compiled patch outputs exceed bounds")
        metadata = json.loads((args.metadata / "partitions.json").read_text(encoding="utf-8"))
        result, receipt = apply_patch(args.image.read_bytes(), metadata,
                                      args.payload.read_bytes(), args.branch.read_bytes(), manifest)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(result)
        args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        print(f"bootloader patch refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
