#!/usr/bin/env python3
"""Tests for the keyless mapped_ab_v1 image patcher."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("bk7258_mapped_ab_apply", ROOT / "apply.py")
apply_tool = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(apply_tool)

BOOT_SIZE = 0x11000
CP_SIZE = 0x11000
AP_SIZE = 0x11000
TRIAL_OFFSET = 0x33000
TRIAL_SIZE = 0x22000
A_LOGICAL = 0x10000
LOADER_LOGICAL = 0x20000
CP_LOGICAL = 0x10000
IMAGE_SIZE = 0x11000
HEAD = A_LOGICAL + LOADER_LOGICAL - 4096
BLANK_UNIT = 0xC000
PAYLOAD = b"\xaa" * 64
# tests/test_mapped_ab.c decodes the same bytes through the bootloader checker.
GOLDEN_DESCRIPTOR = bytes.fromhex(
    "484d41420100000040000000070000000010010000100100002002000030030000200200"
    "001001000080000040000000012615907020000000000000daac6457"
)


def thumb_branch(source: int, target: int) -> bytes:
    immediate = target - (source + 4)
    sign = 1 if immediate < 0 else 0
    i1 = (immediate >> 23) & 1
    i2 = (immediate >> 22) & 1
    j1 = (i1 ^ 1 ^ sign) & 1
    j2 = (i2 ^ 1 ^ sign) & 1
    first = 0xF000 | sign << 10 | ((immediate >> 12) & 0x3FF)
    second = 0x9000 | j1 << 13 | j2 << 11 | ((immediate >> 1) & 0x7FF)
    return struct.pack("<HH", first, second)


def metadata() -> dict:
    return {
        "crc_enable": True,
        "section": [
            {"Name": "primary_bootloader", "Offset": 0, "Size": BOOT_SIZE, "Execute": True},
            {"Name": "primary_cp_app", "Offset": BOOT_SIZE, "Size": CP_SIZE, "Execute": True},
            {"Name": "primary_ap_app", "Offset": BOOT_SIZE + CP_SIZE, "Size": AP_SIZE, "Execute": True},
            {"Name": "s_app", "Offset": TRIAL_OFFSET, "Size": TRIAL_SIZE, "Execute": False},
        ],
    }


def table_record(name: bytes, offset: int, size: int) -> bytes:
    record = bytearray(64)
    record[:4] = apply_tool.TABLE_MAGIC
    record[4:4 + len(name)] = name
    struct.pack_into("<II", record, 52, offset, size)
    return bytes(record)


def seal_loader(plain: bytearray) -> None:
    """Refresh the digests after a test changes bytes in the Loader image."""
    view = plain[A_LOGICAL:A_LOGICAL + IMAGE_SIZE]
    digest = 0x811C9DC5
    for byte in view:
        digest = ((digest ^ byte) * 0x1000193) & 0xFFFFFFFF
    struct.pack_into("<II", plain, HEAD + 76, zlib.crc32(view), digest)
    seal_head(plain)


def seal_head(plain: bytearray) -> None:
    struct.pack_into("<I", plain, HEAD + 92, zlib.crc32(bytes(plain[HEAD:HEAD + 92])))


def native_plain() -> bytearray:
    plain = bytearray(b"\xff" * ((BOOT_SIZE + CP_SIZE + AP_SIZE) // 34 * 32))
    for offset in range(apply_tool.SOURCE_SIZE):
        plain[offset] = (offset * 7 + 3) & 0xFF
    plain[apply_tool.ENTRY_OFFSET:apply_tool.ENTRY_OFFSET + 4] = bytes.fromhex("10b598b0")
    table = table_record(b"bootloader", 0, BOOT_SIZE) + table_record(
        b"appa", BOOT_SIZE, CP_SIZE + AP_SIZE
    ) + table_record(b"s_app", TRIAL_OFFSET, TRIAL_SIZE)
    plain[apply_tool.TABLE_OFFSET:apply_tool.TABLE_OFFSET + len(table)] = table
    for offset in range(IMAGE_SIZE):
        plain[A_LOGICAL + offset] = (offset * 31 + 1) & 0xFF
    base = 0x02010000
    struct.pack_into("<II", plain, A_LOGICAL, 0x28001000, base + 0x101)
    struct.pack_into("<II", plain, A_LOGICAL + CP_LOGICAL, 0x28002000, base + CP_LOGICAL + 0x101)
    plain[HEAD:HEAD + 96] = bytes(96)
    plain[HEAD:HEAD + 4] = b"RBL\0"
    struct.pack_into("<II", plain, HEAD + 84, IMAGE_SIZE, IMAGE_SIZE)
    seal_loader(plain)
    return plain


def encode(plain: bytes | bytearray) -> bytes:
    stored = bytearray(apply_tool.encode_storage(bytes(plain)))
    # Blank units keep their erased CRC bytes in native images.
    at = BLANK_UNIT // 32 * 34
    stored[at:at + 34] = b"\xff" * 34
    return bytes(stored)


def fixture_manifest(plain: bytes | bytearray) -> dict:
    return {
        "source_sha256": hashlib.sha256(bytes(plain[:apply_tool.SOURCE_SIZE])).hexdigest(),
        "payload_sha256": hashlib.sha256(PAYLOAD).hexdigest(),
        "entry_original_hex": "10b598b0",
        "maximum_payload_size": 0x8000,
    }


class ApplyPatchTest(unittest.TestCase):
    def setUp(self) -> None:
        plain = native_plain()
        self.image = encode(plain)
        self.manifest = fixture_manifest(plain)
        self.metadata = metadata()
        self.payload = PAYLOAD
        self.branch = thumb_branch(apply_tool.ENTRY_OFFSET, apply_tool.PAYLOAD_OFFSET)

    def patch(self) -> tuple[bytes, dict]:
        return apply_tool.apply_patch(
            self.image, self.metadata, self.payload, self.branch, self.manifest
        )

    def assert_refused(self, text: str) -> None:
        with self.assertRaisesRegex(apply_tool.PatchError, text):
            self.patch()

    def set_plain(self, plain: bytearray) -> None:
        self.image = encode(plain)
        self.manifest = fixture_manifest(plain)

    def test_patches_only_declared_bootloader_ranges(self) -> None:
        result, receipt = self.patch()
        self.assertEqual(len(result), len(self.image))
        self.assertEqual(result[BOOT_SIZE:], self.image[BOOT_SIZE:])
        before = apply_tool.decode_storage(self.image)
        after = apply_tool.decode_storage(result)
        self.assertEqual(after[0x2070:0x2074], self.branch)
        self.assertEqual(after[0x7F00:0x7F40], GOLDEN_DESCRIPTOR)
        self.assertEqual(after[0x8000:0x8040], self.payload)
        changed = {index for index in range(len(before)) if before[index] != after[index]}
        allowed = set(range(0x2070, 0x2074)) | set(range(0x7F00, 0x7F40)) | set(range(0x8000, 0x8040))
        self.assertLessEqual(changed, allowed)
        self.assertEqual(receipt["contract"], "mapped_ab_v1")
        self.assertEqual(receipt["contract_version"], 1)
        self.assertEqual(receipt["payload_sha256"], hashlib.sha256(self.payload).hexdigest())
        self.assertEqual(receipt["native_image_sha256"], hashlib.sha256(result).hexdigest())
        self.assertEqual(receipt["descriptor_crc32"], zlib.crc32(GOLDEN_DESCRIPTOR[:60]))
        self.assertFalse(receipt["forced_uart_logging"])
        self.assertNotIn("key", json.dumps(receipt).lower())

    def test_rewrites_only_the_stored_units_it_changed(self) -> None:
        result, _ = self.patch()
        touched = {0x2070 // 32, 0x7F00 // 32, 0x7F20 // 32, 0x8000 // 32, 0x8020 // 32}
        for unit in range(BOOT_SIZE // 34):
            at = unit * 34
            if unit in touched:
                self.assertEqual(result[at + 32:at + 34], apply_tool.flash_crc(result[at:at + 32]))
            else:
                self.assertEqual(result[at:at + 34], self.image[at:at + 34], hex(at))
        at = BLANK_UNIT // 32 * 34
        self.assertEqual(result[at:at + 34], b"\xff" * 34)

    def test_patch_is_not_reapplied_to_its_own_output(self) -> None:
        self.image, _ = self.patch()
        self.assert_refused("pinned vendor bootloader")

    def test_refuses_unpinned_bootloader(self) -> None:
        self.manifest["source_sha256"] = "0" * 64
        self.assert_refused("pinned vendor bootloader")

    def test_refuses_changed_checker_instructions(self) -> None:
        plain = native_plain()
        plain[0x2070] = 0
        self.set_plain(plain)
        self.assert_refused("original checker instructions")

    def test_refuses_used_destinations(self) -> None:
        for offset in (0x7F20, 0x8020):
            with self.subTest(offset=hex(offset)):
                plain = native_plain()
                plain[offset] = 0
                self.set_plain(plain)
                self.assert_refused("unused vendor padding")

    def test_refuses_vendor_table_that_disagrees_with_geometry(self) -> None:
        table = apply_tool.TABLE_OFFSET
        cases = {
            "wrong size": (table + 64, table_record(b"appa", BOOT_SIZE, CP_SIZE)),
            "wrong offset": (table + 128, table_record(b"s_app", TRIAL_OFFSET + 0x11000, TRIAL_SIZE)),
            "missing trial": (table + 128, b"\xff" * 64),
            "duplicate": (table + 192, table_record(b"appa", BOOT_SIZE, CP_SIZE + AP_SIZE)),
        }
        for name, (offset, record) in cases.items():
            with self.subTest(name):
                plain = native_plain()
                plain[offset:offset + 64] = record
                self.set_plain(plain)
                self.assert_refused("partition table")

        # The vendor lookup stops at the first invalid record.
        plain = native_plain()
        trial = bytes(plain[table + 128:table + 192])
        plain[table + 128:table + 192] = b"\xff" * 64
        plain[table + 192:table + 256] = trial
        self.set_plain(plain)
        self.assert_refused("partition table")

    def test_refuses_loader_the_checker_would_reject(self) -> None:
        def head_word(offset: int, value: int):
            def change(plain: bytearray) -> None:
                struct.pack_into("<I", plain, HEAD + offset, value)
            return change

        def sizes(value: int):
            def change(plain: bytearray) -> None:
                struct.pack_into("<II", plain, HEAD + 84, value, value)
            return change

        cases = {
            "magic": head_word(0, 0),
            "algorithm": head_word(4, 1),
            "algorithm high byte": head_word(4, 0x100),
            "mismatched sizes": head_word(88, IMAGE_SIZE - 32),
            "zero size": sizes(0),
            "unaligned size": sizes(IMAGE_SIZE + 1),
            "size below AP vector": sizes(CP_LOGICAL),
            "size over head": sizes(LOADER_LOGICAL - 4096 + 32),
        }
        for name, change in cases.items():
            with self.subTest(name):
                plain = native_plain()
                change(plain)
                seal_head(plain)
                self.set_plain(plain)
                self.assert_refused("RBL header")

        plain = native_plain()
        plain[HEAD + 92] ^= 1
        self.set_plain(plain)
        self.assert_refused("RBL header")

    def test_refuses_bad_loader_vectors(self) -> None:
        base = 0x02010000
        for offset, value in (
            (0, 0x20000000),
            (0, 0x28001004),
            (4, base + 0x100),
            (4, base + CP_LOGICAL + 0x101),
            (4, 0x12010101),
            (CP_LOGICAL, 0x280A0008),
            (CP_LOGICAL + 4, base + 0x101),
            (CP_LOGICAL + 4, base + IMAGE_SIZE + 1),
        ):
            with self.subTest(offset=hex(offset), value=hex(value)):
                plain = native_plain()
                struct.pack_into("<I", plain, A_LOGICAL + offset, value)
                seal_loader(plain)
                self.set_plain(plain)
                self.assert_refused("vectors")

    def test_refuses_erased_units_the_checker_reads(self) -> None:
        for logical in (A_LOGICAL + 0x400, HEAD, HEAD + 64):
            with self.subTest(offset=hex(logical)):
                stored = bytearray(self.image)
                at = logical // 32 * 34
                stored[at:at + 34] = b"\xff" * 34
                self.image = bytes(stored)
                with self.assertRaises(apply_tool.PatchError):
                    self.patch()
                self.setUp()

    def test_refuses_erased_header_middle_with_valid_digests(self) -> None:
        plain = native_plain()
        plain[HEAD + 32:HEAD + 64] = b"\xff" * 32
        seal_head(plain)
        self.set_plain(plain)
        stored = bytearray(self.image)
        at = (HEAD + 32) // 32 * 34
        stored[at:at + 34] = b"\xff" * 34
        self.image = bytes(stored)
        self.assert_refused("erased units")

    def test_refuses_each_loader_digest_independently(self) -> None:
        for offset in (76, 80):
            with self.subTest(offset=offset):
                plain = native_plain()
                plain[HEAD + offset] ^= 1
                seal_head(plain)
                self.set_plain(plain)
                self.assert_refused("digests")

    def test_refuses_invalid_payload_sizes(self) -> None:
        for payload in (b"", b"\xaa" * 0x8001):
            with self.subTest(size=len(payload)):
                self.payload = payload
                self.assert_refused("bootloader window")

    def test_refuses_payload_past_physical_bootloader_window(self) -> None:
        self.manifest["maximum_payload_size"] = 0x10000
        self.payload = b"\xaa" * 0x8001
        self.assert_refused("bootloader window")

    def test_refuses_unqualified_payload(self) -> None:
        self.payload = b"\xbb" * 64
        self.assert_refused("qualified contract")

    def test_refuses_wrong_branch(self) -> None:
        for branch, text in (
            (b"\x00\x00\x00", "exactly four bytes"),
            (b"\x00\xbf\x00\xbf", "Thumb B.W"),
            (thumb_branch(apply_tool.ENTRY_OFFSET, 0x8004), "outside the payload entry"),
        ):
            with self.subTest(branch=branch.hex()):
                self.branch = branch
                self.assert_refused(text)

    def test_refuses_storage_crc_error(self) -> None:
        image = bytearray(self.image)
        image[0] ^= 1
        self.image = bytes(image)
        self.assert_refused("storage CRC")

    def test_refuses_truncated_storage(self) -> None:
        self.image = self.image[:-34]
        self.assert_refused("contiguous")

    def test_refuses_unsupported_geometry(self) -> None:
        def mutate(index: int, **changes: object):
            def apply(data: dict) -> None:
                data["section"][index].update(changes)
            return apply

        def gap_before_ap(data: dict) -> None:
            data["section"][2]["Offset"] += 0x11000
            data["section"][3]["Offset"] += 0x11000

        def gap_before_cp(data: dict) -> None:
            data["section"][1]["Offset"] += 0x11000
            data["section"][2]["Offset"] += 0x11000
            data["section"][3]["Offset"] += 0x11000

        def missing_trial(data: dict) -> None:
            data["section"].pop()

        def duplicate(data: dict) -> None:
            data["section"].append(dict(data["section"][3]))

        cases = {
            "crc disabled": (lambda data: data.update(crc_enable=False), "CRC-enabled"),
            "section not a list": (lambda data: data.update(section={}), "partition list"),
            "missing trial": (missing_trial, "bootloader, CP, AP and trial"),
            "duplicate": (duplicate, "duplicate"),
            "boolean offset": (mutate(1, Offset=True), "duplicate"),
            "out of bounds": (mutate(3, Size=apply_tool.LIMIT), "flash bounds"),
            "overlap": (mutate(3, Offset=0x22000), "overlap"),
            "gap before CP": (gap_before_cp, "contiguous"),
            "gap before AP": (gap_before_ap, "contiguous"),
            "image shorter than AP": (mutate(2, Size=0x8800), "contiguous"),
            "trial executable": (mutate(3, Execute=True), "contiguous"),
            "loader not executable": (mutate(1, Execute=False), "contiguous"),
            "unaligned trial": (mutate(3, Offset=TRIAL_OFFSET + 0x1000), "aligned"),
            "trial too small": (mutate(3, Size=0x11000), "windows"),
        }
        for name, (change, text) in cases.items():
            with self.subTest(name):
                self.metadata = copy.deepcopy(metadata())
                change(self.metadata)
                self.assert_refused(text)


class ManifestAndCliTest(unittest.TestCase):
    def write_inputs(self, root: Path) -> list[str]:
        (root / "image.bin").write_bytes(encode(native_plain()))
        (root / "payload.bin").write_bytes(PAYLOAD)
        (root / "branch.bin").write_bytes(
            thumb_branch(apply_tool.ENTRY_OFFSET, apply_tool.PAYLOAD_OFFSET))
        (root / "metadata").mkdir()
        (root / "metadata/partitions.json").write_text(json.dumps(metadata()), encoding="utf-8")
        return [
            "--manifest", str(ROOT / "manifest.json"),
            "--image", str(root / "image.bin"),
            "--metadata", str(root / "metadata"),
            "--payload", str(root / "payload.bin"),
            "--branch", str(root / "branch.bin"),
            "--output", str(root / "out/all-app.bin"),
            "--receipt", str(root / "out/bootloader.json"),
        ]

    def test_manifest_pins_current_contract(self) -> None:
        manifest = apply_tool.load_manifest(ROOT / "manifest.json")
        self.assertEqual(manifest["source_sha256"], apply_tool.SOURCE_SHA256)
        self.assertEqual(manifest["payload_sha256"], apply_tool.PAYLOAD_SHA256)
        self.assertFalse(manifest["logging_changes"])

    def test_manifest_addresses_match_the_checker_sources(self) -> None:
        manifest = apply_tool.load_manifest(ROOT / "manifest.json")
        source = (ROOT / "mapped_ab.c").read_text(encoding="utf-8")
        linker = (ROOT / "mapped_ab.ld").read_text(encoding="utf-8")
        self.assertIn(f"0x{manifest['vendor_lookup_thumb_address']:08x}u", source)
        self.assertIn(f"0x{0x02000000 + manifest['entry_offset']:08x}", linker)
        self.assertIn(f"0x{0x02000000 + manifest['payload_offset']:08x}", linker)

    def test_manifest_rejects_changed_entry(self) -> None:
        data = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))
        data["entry_offset"] += 4
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(apply_tool.PatchError):
                apply_tool.load_manifest(path)

    def test_cli_publishes_image_and_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            arguments = self.write_inputs(root)
            manifest = fixture_manifest(native_plain())
            with mock.patch.object(apply_tool, "load_manifest", return_value=manifest):
                self.assertEqual(apply_tool.main(arguments), 0)
            image = (root / "out/all-app.bin").read_bytes()
            receipt = json.loads((root / "out/bootloader.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["native_image_sha256"], hashlib.sha256(image).hexdigest())

    def test_cli_refuses_without_publishing_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # The synthetic image is not the pinned vendor image.
            self.assertEqual(apply_tool.main(self.write_inputs(root)), 1)
            self.assertFalse((root / "out/all-app.bin").exists())
            self.assertFalse((root / "out/bootloader.json").exists())


if __name__ == "__main__":
    unittest.main()
