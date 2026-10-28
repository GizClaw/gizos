"""Offline BK7258 legacy AB recovery transform; never acquires device secrets."""

import json
import struct
import zlib
from pathlib import Path

from Crypto.Cipher import AES


class ImageError(ValueError):
    """An unsupported or inconsistent recovery input."""


def _swap_words(data: bytes) -> bytes:
    return b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))


class BekenXTS:
    """32-byte Beken blocks at logical byte addresses, with word-swapped data."""

    def __init__(self, key: bytes):
        if len(key) != 32:
            raise ImageError("KEY_LENGTH")
        self._data = AES.new(key[:16], AES.MODE_ECB)
        self._tweak = AES.new(key[16:], AES.MODE_ECB)

    def block(self, data: bytes, address: int, decrypt: bool = False) -> bytes:
        if len(data) != 32 or type(address) is not int or address < 0 or address % 32 or address > 0xFFFFFFE0:
            raise ImageError("BLOCK_ADDRESS_OR_LENGTH")
        tweak = self._tweak.encrypt(address.to_bytes(16, "big"))
        data = _swap_words(data)
        output = bytearray()
        for offset in (0, 16):
            value = bytes(a ^ b for a, b in zip(data[offset:offset + 16], tweak))
            value = self._data.decrypt(value) if decrypt else self._data.encrypt(value)
            output.extend(a ^ b for a, b in zip(value, tweak))
            number = int.from_bytes(tweak, "big")
            number = ((number << 1) & ((1 << 128) - 1)) ^ (0x87 if number >> 127 else 0)
            tweak = number.to_bytes(16, "big")
        return _swap_words(output)


def flash_crc(data: bytes) -> bytes:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x8005 if crc & 0x8000 else 0)) & 0xFFFF
    return crc.to_bytes(2, "big")


def _logical(physical: int) -> int:
    if physical % 34:
        raise ImageError("LAYOUT_CRC_ALIGNMENT")
    return physical // 34 * 32


def validate_layout(metadata: Path, image_size: int) -> list[tuple[int, int]]:
    """Accept only contiguous legacy Bootloader/CP/AP code starting at Flash zero."""
    table = json.loads((metadata / "partitions.json").read_text(encoding="utf-8"))
    if not isinstance(table, dict):
        raise ImageError("LAYOUT_FORMAT")
    if table.get("crc_enable") is not True or not isinstance(table.get("section"), list):
        raise ImageError("LAYOUT_FORMAT")
    sections = table["section"]
    if any(not isinstance(part, dict) for part in sections):
        raise ImageError("LAYOUT_FORMAT")
    ranges = []
    end = 0
    for name in ("primary_bootloader", "primary_cp_app", "primary_ap_app"):
        matches = [part for part in sections if part.get("Name") == name]
        if len(matches) != 1:
            raise ImageError("LAYOUT_CODE_PARTITIONS")
        part = matches[0]
        start, size = part.get("Offset"), part.get("Size")
        if type(start) is not int or type(size) is not int or start != end or size <= 0 or part.get("Execute") is not True:
            raise ImageError("LAYOUT_CODE_RANGE")
        end = start + size
        logical_start, logical_end = _logical(start), _logical(end)
        if logical_start % 4096 or logical_end % 4096:
            raise ImageError("LAYOUT_PAGE_ALIGNMENT")
        ranges.append((logical_start, logical_end))
    for part in sections:
        start, size = part.get("Offset"), part.get("Size")
        if type(start) is not int or type(size) is not int or start < 0 or size <= 0:
            raise ImageError("LAYOUT_RANGE")
        if part.get("Name") not in ("primary_bootloader", "primary_cp_app", "primary_ap_app"):
            if part.get("Execute") is True or start < end:
                raise ImageError("LAYOUT_OVERLAP_OR_EXECUTABLE")
    if end != image_size or end > 16 * 1024 * 1024:
        raise ImageError("LAYOUT_IMAGE_LENGTH")
    return ranges


def check_rbl(plain: bytes, payload_start: int, ap_start: int) -> int:
    if len(plain) < 4096 + payload_start:
        raise ImageError("RBL_LENGTH")
    header_start = len(plain) - 4096
    header = plain[header_start:header_start + 96]
    if header[:4] != b"RBL\0" or header[4:6] != b"\0\0":
        raise ImageError("RBL_MAGIC")
    crc, fnv, raw_size, package_size, header_crc = struct.unpack_from("<5I", header, 76)
    payload_end = payload_start + raw_size
    if (
        raw_size == 0 or raw_size % 32 or raw_size != package_size
        or payload_end < ap_start + 8 or payload_end > header_start
    ):
        raise ImageError("RBL_PAYLOAD_LENGTH")
    if plain[payload_end:header_start] != b"\xff" * (header_start - payload_end) or plain[header_start + 96:] != b"\xff" * (4096 - 96):
        raise ImageError("RBL_PADDING")
    payload = plain[payload_start:payload_start + raw_size]
    value = 0x811C9DC5
    for byte in payload:
        value = ((value ^ byte) * 0x1000193) & 0xFFFFFFFF
    if value != fnv or zlib.crc32(payload) != crc or zlib.crc32(header[:92]) != header_crc:
        raise ImageError("RBL_DIGEST")
    return payload_end


def encrypt_recovery(source: bytes, metadata: Path, key: bytes) -> bytes:
    """Validate and transform a complete supported native image without publishing it."""
    ranges = validate_layout(metadata, len(source))
    blocks = []
    for offset in range(0, len(source), 34):
        packet = source[offset:offset + 34]
        if len(packet) != 34 or (packet != b"\xff" * 34 and flash_crc(packet[:32]) != packet[32:]):
            raise ImageError("SOURCE_CRC")
        blocks.append(packet[:32])
    plain = b"".join(blocks)
    if plain[0x100:0x106] != b"BK7236":
        raise ImageError("BOOTROM_MARKER")
    payload_end = check_rbl(plain, ranges[1][0], ranges[2][0])
    # Only the legacy XIP startup contract is supported: each reset handler is
    # a Thumb address inside its own Flash load range (secure or NS alias).
    for index, (start, end) in enumerate(ranges):
        # AP/CP reset targets must be covered by RBL integrity checks, not
        # merely located somewhere in a partition that also holds the trailer.
        code_end = end if index == 0 else min(end, payload_end)
        stack, reset = struct.unpack_from("<II", plain, start)
        flash_address = reset & ~0x10000000
        logical_reset = (flash_address & ~1) - 0x02000000
        if stack in (0, 0xFFFFFFFF) or stack % 8 or not reset & 1 or not start <= logical_reset < code_end:
            raise ImageError("CODE_VECTOR_LAYOUT")
    cipher = BekenXTS(key)
    encrypted = bytearray(b"".join(cipher.block(plain[i:i + 32], i) for i in range(0, len(plain), 32)))
    encrypted[0x100:0x110] = plain[0x100:0x110]
    output = b"".join(encrypted[i:i + 32] + flash_crc(encrypted[i:i + 32]) for i in range(0, len(encrypted), 32))
    if len(output) != len(source):
        raise ImageError("OUTPUT_LENGTH")
    decoded = b"".join(cipher.block(encrypted[i:i + 32], i, True) for i in range(0, len(encrypted), 32))
    if decoded[:0x100] != plain[:0x100] or decoded[0x110:] != plain[0x110:]:
        raise ImageError("OUTPUT_DECRYPT")
    check_rbl(decoded, ranges[1][0], ranges[2][0])
    return output
