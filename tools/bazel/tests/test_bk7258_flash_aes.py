"""Public synthetic recovery and independently captured vendor known answers."""
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from tools.bazel.bk7258_flash_aes import BekenXTS, ImageError, encrypt_recovery, encrypt_app, validate_app, flash_crc


def synthetic_image(root):
    sections = [
        dict(Name="primary_bootloader", Offset=0, Size=4352, Execute=True),
        dict(Name="primary_cp_app", Offset=4352, Size=4352, Execute=True),
        dict(Name="primary_ap_app", Offset=8704, Size=8704, Execute=True),
    ]
    (root / "partitions.json").write_text(json.dumps(dict(crc_enable=True, section=sections)))
    plain = bytearray(b"\xff" * 16384)
    for start in (0, 4096, 8192):
        struct.pack_into("<II", plain, start, 0x28030000, 0x02000000 + start + 0x81)
    plain[256:272] = b"BK7236-test-only"
    payload = plain[4096:12288]
    fnv = 0x811C9DC5
    for b in payload:
        fnv = ((fnv ^ b) * 0x1000193) & 0xFFFFFFFF
    header = bytearray(96)
    header[:4] = b"RBL\0"
    struct.pack_into("<4I", header, 76, zlib.crc32(payload), fnv, len(payload), len(payload))
    struct.pack_into("<I", header, 92, zlib.crc32(header[:92]))
    plain[12288:12384] = header
    source = b"".join(plain[i:i+32] + flash_crc(plain[i:i+32]) for i in range(0, len(plain), 32))
    return source, plain


class FlashAesTests(unittest.TestCase):
    def test_managed_app_at_nonzero_address(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, full = synthetic_image(root)
            plain = full[4096:]
            base = 0x240000
            for offset in (0, 4096):
                struct.pack_into("<I", plain, offset + 4, 0x02000000 + base + offset + 0x81)
            payload = plain[:8192]
            fnv = 0x811C9DC5
            for value in payload:
                fnv = ((fnv ^ value) * 0x1000193) & 0xFFFFFFFF
            struct.pack_into("<II", plain, 8192 + 76, zlib.crc32(payload), fnv)
            struct.pack_into("<I", plain, 8192 + 92, zlib.crc32(plain[8192:8192 + 92]))
            table = {"crc_enable": True, "section": [
                dict(Name="primary_cp_app", Offset=0x264000, Size=4352, Execute=True),
                dict(Name="primary_ap_app", Offset=0x264000+4352, Size=8704, Execute=True),
            ]}
            source = b"".join(plain[i:i+32]+flash_crc(plain[i:i+32]) for i in range(0,len(plain),32))
            self.assertEqual(validate_app(source,table),(plain,base,4096))
            result = encrypt_app(source,table,bytes(range(32)))
            cipher = BekenXTS(bytes(range(32)))
            restored = b"".join(cipher.block(result[i:i+32],base+i//34*32,True) for i in range(0,len(result),34))
            self.assertEqual(restored,plain)
            self.assertNotEqual(result,source)
            with self.assertRaises(ImageError):
                encrypt_app(result,table,bytes(range(32)))
            for start in (0, 0x264000+34):
                bad = json.loads(json.dumps(table))
                bad["section"][0]["Offset"] = start
                with self.assertRaises(ImageError):
                    validate_app(source,bad)
            with self.assertRaises(ImageError):
                validate_app(source[:-1],table)

    def test_vendor_known_answers(self):
        fixture = Path(__file__).parent / "fixtures/bk7258_flash_aes_vectors.json"
        if "TEST_SRCDIR" in os.environ:
            from python.runfiles import Runfiles
            fixture = Path(Runfiles.Create().Rlocation("gizos/tools/bazel/tests/fixtures/bk7258_flash_aes_vectors.json"))
        data = json.loads(fixture.read_text())
        cipher = BekenXTS(bytes.fromhex(data["key_hex"]))
        for row in data["vectors"]:
            with self.subTest(address=row["address"]):
                block = bytes.fromhex(data["input_hex"])
                encrypted = cipher.block(block, row["address"])
                self.assertEqual(encrypted.hex(), row["ciphertext"])
                self.assertEqual(cipher.block(encrypted, row["address"], True), block)
        for row in data["native_cases"]:
            block = bytes.fromhex(row["input_hex"])
            block += b"\xff" * ((-len(block)) % 32)
            address = int(row["address"], 0)
            encrypted = b"".join(cipher.block(block[i:i+32], address+i) for i in range(0, len(block), 32))
            self.assertEqual(encrypted.hex(), row["native_ciphertext_hex"])
            self.assertEqual(b"".join(encrypted[i:i+32]+flash_crc(encrypted[i:i+32]) for i in range(0,len(encrypted),32)).hex(), row["native_crc_hex"])

    def test_complete_recovery_preserves_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, plain = synthetic_image(root)
            key = bytes(range(32))
            output = encrypt_recovery(source, root, key)
            self.assertEqual(len(output), len(source))
            encoded = b"".join(output[i:i+32] for i in range(0,len(output),34))
            self.assertEqual(encoded[256:272], plain[256:272])
            for i in range(0,len(output),34):
                self.assertEqual(flash_crc(output[i:i+32]), output[i+32:i+34])
            cipher = BekenXTS(key)
            decoded = b"".join(cipher.block(encoded[i:i+32],i,True) for i in range(0,len(encoded),32))
            self.assertEqual(decoded[:256], plain[:256])
            self.assertEqual(decoded[272:], plain[272:])

    def test_rejects_corruption_and_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = synthetic_image(root)
            for bad in [source[:-1], bytes([source[0]^1])+source[1:]]:
                with self.assertRaises(ImageError):
                    encrypt_recovery(bad, root, bytes(range(32)))
            table = json.loads((root/'partitions.json').read_text())
            table['section'][1]['Offset'] += 34
            (root/'partitions.json').write_text(json.dumps(table))
            with self.assertRaises(ImageError):
                encrypt_recovery(source, root, bytes(range(32)))

    def test_rejects_marker_and_rbl_even_with_valid_crc(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, original = synthetic_image(root)
            for offset in [256, 12288, 4096, 12288+84]:
                plain = original.copy()
                plain[offset] ^= 1
                source = b''.join(plain[i:i+32]+flash_crc(plain[i:i+32]) for i in range(0,len(plain),32))
                with self.assertRaises(ImageError):
                    encrypt_recovery(source, root, bytes(range(32)))

    def test_rejects_layout_or_rbl_inconsistency_with_valid_checksums(self):
        def encode(plain):
            return b"".join(plain[i:i+32]+flash_crc(plain[i:i+32]) for i in range(0,len(plain),32))

        def resign(plain, size):
            payload = plain[4096:4096+size]
            fnv = 0x811C9DC5
            for byte in payload:
                fnv = ((fnv ^ byte) * 0x1000193) & 0xFFFFFFFF
            struct.pack_into("<4I", plain, 12288+76, zlib.crc32(payload), fnv, size, size)
            struct.pack_into("<I", plain, 12288+92, zlib.crc32(plain[12288:12288+92]))

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, original = synthetic_image(root)
            table = json.loads((root/"partitions.json").read_text())
            table["section"][1]["Size"] += 4352
            table["section"][2]["Offset"] += 4352
            table["section"][2]["Size"] -= 4352
            (root/"partitions.json").write_text(json.dumps(table))
            with self.assertRaises(ImageError):
                encrypt_recovery(source, root, bytes(range(32)))
            synthetic_image(root)
            for kind in ("wrong_vector", "trailer_reset", "short_payload", "uncovered_data", "header_padding"):
                plain = original.copy()
                if kind == "wrong_vector":
                    struct.pack_into("<I", plain, 8192+4, 0x02000081)
                    resign(plain,8192)
                elif kind == "trailer_reset":
                    struct.pack_into("<I", plain, 8192+4, 0x02000000+12288+0x81)
                    resign(plain,8192)
                elif kind == "short_payload":
                    resign(plain,32)
                elif kind == "uncovered_data":
                    plain[12256] = 1
                    resign(plain,8192-32)
                else:
                    plain[12288+96] = 1
                with self.subTest(kind=kind), self.assertRaises(ImageError):
                    encrypt_recovery(encode(plain), root, bytes(range(32)))

    def test_block_rejections(self):
        for size in [0,16,31,33,64]:
            with self.assertRaises(ImageError):
                BekenXTS(bytes(size))
        cipher = BekenXTS(bytes(range(32)))
        for address in [-32,1,0x100000000,True]:
            with self.assertRaises(ImageError):
                cipher.block(bytes(32),address)


if __name__ == '__main__':
    unittest.main()
