"""Check the R36S BootROM and miniloader inputs read back from a card image."""

import hashlib
from pathlib import Path
import struct
import sys
import unittest
import zlib

MIB = 1024 * 1024
SECTOR = 512
# ROCKNIX 20261101-a, from LBA 64 through the last byte of the pinned miniloader.
IDB_SHA256 = "2fc42ada721b84d8bd40a9a3486abde92d869a5124cad14789743205b1168384"


def rk_crc32(payload):
    # Rockchip loaderimage uses a non-reflected CRC with init=0 and this polynomial.
    table = []
    for byte in range(256):
        crc = byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C10DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
        table.append(crc)
    crc = 0
    for byte in payload:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ table[(crc >> 24) ^ byte]
    return crc


def check_loader(loader):
    primary = loader[:MIB]
    assert len(loader) == 4 * MIB and loader == primary * 4, "U-Boot copies"
    magic, version, reserved, addr, size, crc, digest_size = struct.unpack_from("<8s6I", primary)
    assert (magic, version, reserved, addr, digest_size) == (b"LOADER  ", 0, 0, 0x200000, 32), "LOADER header"
    assert 0 < size <= MIB - 2048, "U-Boot length"
    payload = primary[2048:2048 + size]
    assert rk_crc32(payload) == crc, "U-Boot CRC"
    assert hashlib.sha256(payload + primary[16:24] + primary[28:32]).digest() == primary[32:64], "U-Boot SHA-256"


def elf_segments(elf):
    assert elf[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", elf, 18)[0] == 183, "AArch64 BL31 ELF"
    offset, = struct.unpack_from("<Q", elf, 32)
    size, count = struct.unpack_from("<HH", elf, 54)
    assert size == 56 and offset + count * size <= len(elf), "ELF program header extent"
    segments = []
    for i in range(count):
        kind, _, start, vaddr, paddr, filesz, _, _ = struct.unpack_from("<II6Q", elf, offset + i * size)
        if kind == 1:
            assert filesz > 0 and start + filesz <= len(elf), "ELF PT_LOAD extent"
            segments.append((vaddr, paddr, elf[start:start + filesz]))
    return segments


def check_trust(trust, segments, golden):
    primary = trust[:2 * MIB]
    assert len(trust) == 4 * MIB and trust == primary * 2, "BL31 backup copy"
    magic, version, flags, size = struct.unpack_from("<4sIII", primary)
    count, sign = size >> 16, (size & 0xFFFF) * 4
    assert (magic, version, flags, count, sign) == (b"BL3X", 0, 0x23, len(segments), 0x320 + 48 * len(segments)), "BL3X component table"
    next_sector = 4
    for i, (_, paddr, segment) in enumerate(segments):
        desc = 0x320 + 48 * i
        digest, addr, padded_size, _, _ = struct.unpack_from("<32s4I", primary, desc)
        kind, start, sectors, _ = struct.unpack_from("<4sIII", primary, sign + 256 + 16 * i)
        assert addr == paddr, f"BL31[{i}] physical load address {addr:#x} != {paddr:#x}"
        expected_sectors = ((len(segment) + 2047) // 2048) * 4
        assert padded_size == sectors == expected_sectors, f"BL31[{i}] sector count"
        assert kind == b"BL31" and start == next_sector, f"BL31[{i}] storage address"
        next_sector += sectors
        payload = primary[start * SECTOR:next_sector * SECTOR]
        assert payload == segment.ljust(sectors * SECTOR, b"\0"), f"BL31[{i}] ELF contents"
        assert hashlib.sha256(payload).digest() == digest, f"BL31[{i}] segment digest"
    assert hashlib.sha256(trust).hexdigest() == golden, "4 MiB trust region differs from ROCKNIX 20261101-a"


def check_script(script, commands):
    header, payload = script[:64], script[64:]
    magic, hcrc, _, size, load, entry, dcrc, os_id, arch, kind, compression = struct.unpack_from(">7I4B", header)
    assert (magic, size, load, entry) == (0x27051956, len(payload), 0, 0), "U-Boot script header"
    assert (os_id, arch, kind, compression) == (5, 22, 6, 0), "U-Boot image type"
    assert zlib.crc32(header[:4] + bytes(4) + header[8:]) == hcrc, "script header CRC"
    assert zlib.crc32(payload) == dcrc, "script data CRC"
    assert struct.unpack_from(">II", payload) == (len(commands), 0) and payload[8:] == commands, "boot.cmd contents"


def check_regressions(trust, segments, golden, script, commands):
    case = unittest.TestCase()
    # Both old trust copies had correct payload digests even when the load table used VMAs.
    wrong = bytearray(trust[:2 * MIB])
    for i, (vaddr, _, _) in enumerate(segments):
        struct.pack_into("<I", wrong, 0x320 + 48 * i + 32, vaddr)
    with case.assertRaisesRegex(AssertionError, "physical load address"):
        check_trust(wrong * 2, segments, golden)
    wrong = bytearray(trust[:2 * MIB])
    struct.pack_into("<I", wrong, 0x320 + 36, 0)
    with case.assertRaisesRegex(AssertionError, "sector count"):
        check_trust(wrong * 2, segments, golden)
    with case.assertRaisesRegex(AssertionError, "component table"):
        check_trust((b"BL3X" + bytes(2 * MIB - 4)) * 2, segments, golden)
    wrong = bytearray(script)
    wrong[30] = 2  # IH_TYPE_KERNEL with an otherwise valid header and command stream.
    struct.pack_into(">I", wrong, 4, zlib.crc32(wrong[:4] + bytes(4) + wrong[8:64]))
    with case.assertRaisesRegex(AssertionError, "image type"):
        check_script(wrong, commands)
    print("Rejected incorrect physical addresses, sector counts, signature-only trust and non-script boot image")


def main():
    image, bl31, trust_pin, boot_scr, boot_cmd = map(Path, sys.argv[1:])
    with image.open("rb") as card:
        card.seek(64 * SECTOR)
        assert hashlib.sha256(card.read(176452)).hexdigest() == IDB_SHA256, "IDB, DDR or miniloader differs"
        card.seek(8 * MIB)
        check_loader(card.read(4 * MIB))
        trust = card.read(4 * MIB)
    segments = elf_segments(bl31.read_bytes())
    golden = trust_pin.read_text().split()[0]
    check_trust(trust, segments, golden)
    script, commands = boot_scr.read_bytes(), boot_cmd.read_bytes()
    check_script(script, commands)
    print(f"BL31 primary and backup match ELF PT_LOAD segments and ROCKNIX golden {golden}")
    for i, (_, addr, data) in enumerate(segments):
        print(f"  BL31[{i}] physical={addr:#010x} bytes={len(data)}")
    check_regressions(trust, segments, golden, script, commands)


if __name__ == "__main__":
    main()
