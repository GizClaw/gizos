"""Validate actual :encrypted_package outputs against synthetic native inputs."""

import hashlib
import json
from pathlib import Path
import shlex
import struct
import sys
import unittest

from projects.h2loader.tools.bazel.firmware_artifacts import FACTORY_IDENTITY_SIZE, read_managed_package
from tools.bazel.bk7258_flash_aes import BekenXTS, flash_crc

FILES = [Path(path) for argument in sys.argv[1:] for path in shlex.split(argument)]
BLOCK = 34


def named(suffix: str, directory: str) -> Path:
    found = [path for path in FILES if path.parent.name == directory and path.name.endswith(suffix)]
    if len(set(found)) != 1:
        raise AssertionError(f"{directory}: expected one *{suffix}, found {found}")
    return found[0]


def key() -> bytes:
    return Path("projects/h2loader/tools/bazel/tests/fixtures/flash_aes_public_test.key").read_bytes()


def decrypt(image: bytes, logical_base: int, device_key: bytes | None = None) -> bytes:
    cipher = BekenXTS(device_key or key())
    plain = bytearray()
    for offset in range(0, len(image), BLOCK):
        block, crc = image[offset:offset + 32], image[offset + 32:offset + BLOCK]
        if flash_crc(block) != crc:
            raise AssertionError(f"stored CRC at {offset}")
        plain += cipher.block(block, logical_base + offset // BLOCK * 32, True)
    return bytes(plain)


def plain_units(image: bytes) -> bytes:
    return b"".join(image[i:i + 32] for i in range(0, len(image), BLOCK))


class EncryptedPackageRulesTest(unittest.TestCase):
    def check_package(self, directory, native, suffix, role, image, logical_base, package_format):
        package = named(suffix, directory)
        metadata = json.loads(named(".firmware.json", directory).read_text())
        receipt = json.loads(named(".flash-aes.json", directory).read_text())
        encrypted = named(".flash-aes.rbl", directory).read_bytes()
        source = named("app_ab_crc.rbl", native).read_bytes()
        parsed = read_managed_package(package.read_bytes())

        self.assertEqual(parsed.app, encrypted)
        self.assertNotEqual(parsed.app, source)
        self.assertEqual(decrypt(parsed.app, logical_base), plain_units(source))
        other_key = bytes(byte ^ 1 for byte in key())
        self.assertNotEqual(decrypt(parsed.app, logical_base, other_key), plain_units(source))
        self.assertEqual(parsed.manifest["role"], role)
        self.assertEqual(parsed.manifest["version"], metadata["version"])
        self.assertEqual(metadata["image"], image)
        self.assertEqual(metadata["role"], role)
        self.assertEqual(metadata["package_format"], package_format)
        self.assertEqual(metadata["entry"], "projects/h2loader/tools/bazel")

        # Every published digest describes ciphertext.
        image_digest = hashlib.sha256(parsed.app).hexdigest()
        self.assertEqual(metadata["package_manifest"]["image_sha256"], image_digest)
        self.assertEqual(metadata["assets"][0]["sha256"], hashlib.sha256(package.read_bytes()).hexdigest())
        artifacts = {item["name"]: item["sha256"] for item in metadata["native_artifacts"]}
        self.assertEqual(artifacts["app_ab_crc.rbl"], image_digest)
        debug = {"ap/firmware.elf", "ap/firmware.map", "cp/firmware.elf", "cp/firmware.map"}
        flash = {"app_ab_crc.rbl", "all-app.bin"} if role == "h2loader" else {"app_ab_crc.rbl"}
        self.assertEqual(set(artifacts), debug | flash)

        self.assertEqual(receipt["format"], "bk7258-flash-aes-package-v1")
        self.assertNotIn("bootloader_contract", receipt)
        self.assertEqual(receipt["logical_base"], logical_base)
        self.assertEqual(receipt["physical_base"], logical_base // 32 * BLOCK)
        self.assertEqual(receipt["image_sha256"], image_digest)
        self.assertEqual(receipt["source_image_sha256"], hashlib.sha256(source).hexdigest())
        self.assertEqual(receipt["package_sha256"], hashlib.sha256(package.read_bytes()).hexdigest())
        return parsed, metadata, receipt

    def test_app_packages_keep_data_and_have_no_recovery(self):
        for directory, suffix, package_format in (
            ("encrypted_rules_app_plain_encrypted", ".update.tar.zlib", 1),
            ("encrypted_rules_app_plain_format2_encrypted", ".update.tar", 2),
        ):
            with self.subTest(directory=directory):
                parsed, metadata, receipt = self.check_package(
                    directory, "encrypted_rules_app_native", suffix, "app", "app", 0x240000, package_format
                )
                self.assertEqual([entry.data for entry in parsed.data], [b"unchanged package data\n"])
                self.assertEqual([asset["operation"] for asset in metadata["assets"]], ["managed-install"])
                self.assertNotIn("recovery_sha256", receipt)

    def test_loader_packages_cover_the_mfg_image_and_recovery(self):
        for directory, suffix, package_format in (
            ("encrypted_rules_loader_plain_encrypted", ".update.tar.zlib", 1),
            ("encrypted_rules_loader_plain_format2_encrypted", ".update.tar", 2),
        ):
            with self.subTest(directory=directory):
                parsed, metadata, receipt = self.check_package(
                    directory, "encrypted_rules_loader_native", suffix, "h2loader", "mfg", 0x1000, package_format
                )
                self.assertEqual(parsed.data, [])
                self.assertEqual([asset["operation"] for asset in metadata["assets"]], ["managed-install", "recovery"])

                recovery = named(".flash-aes.bin", directory).read_bytes()
                source = named("all-app.bin", "encrypted_rules_loader_native").read_bytes()
                self.assertNotEqual(recovery, source)
                decoded = decrypt(recovery, 0)
                expected = plain_units(source)
                # The BootROM marker at logical 0x100 stays in the clear; its
                # stored offset includes one CRC per preceding 32-byte unit.
                self.assertEqual(decoded[:0x100], expected[:0x100])
                self.assertEqual(decoded[0x110:], expected[0x110:])
                self.assertEqual(recovery[0x110:0x116], b"BK7236")

                bundle_path = named(".recovery.h2fb", directory)
                bundle = bundle_path.read_bytes()
                flash_offset, data_offset, length = struct.unpack_from("<IQQ", bundle, 20 + 2 * FACTORY_IDENTITY_SIZE)
                self.assertEqual(flash_offset, 0)
                self.assertEqual(bundle[data_offset:data_offset + length], recovery)
                self.assertEqual(receipt["recovery_sha256"], hashlib.sha256(bundle).hexdigest())
                self.assertEqual(metadata["assets"][1]["sha256"], hashlib.sha256(bundle).hexdigest())
                artifacts = {item["name"]: item["sha256"] for item in metadata["native_artifacts"]}
                self.assertEqual(artifacts["all-app.bin"], hashlib.sha256(recovery).hexdigest())

    def test_no_output_embeds_the_key(self):
        secret = key()
        for path in set(FILES):
            if path.is_file() and path.name != "flash_aes_public_test.key":
                self.assertNotIn(secret, path.read_bytes(), path)
                self.assertNotIn(secret.hex().encode(), path.read_bytes(), path)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
