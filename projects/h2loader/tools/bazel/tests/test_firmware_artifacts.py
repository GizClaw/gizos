from __future__ import annotations

from pathlib import Path
import hashlib
import os
import struct
import tempfile
import unittest

from projects.h2loader.tools.bazel.firmware_artifacts import BundleEntry, write_factory_bundle, write_package


class FirmwareArtifactsTest(unittest.TestCase):
    def test_canonical_h2loader_package_digest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "update.tar.zlib"
            write_package(
                output,
                "app/esp/app.bin",
                bytes((0xE9, 1, 2, 3, 4, 5)),
                [BundleEntry("data/a.txt", b"alpha"), BundleEntry("data/z.bin", bytes((0, 1, 2)))],
                role="app",
                board="fixture",
                target="host",
                version="0",
            )
            fixture = (
                Path(os.environ["TEST_SRCDIR"])
                / os.environ["TEST_WORKSPACE"]
                / "projects/h2loader/tools/bazel/tests/fixtures/h2loader_format1.sha256"
            ).read_text(encoding="ascii").strip()
            self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), fixture)

    def test_writes_esp_factory_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "app.bin"
            image.write_bytes(b"app0")
            output = root / "loader.h2fb"
            write_factory_bundle(
                output,
                driver=1,
                board="devkit",
                target="esp32s3",
                baud=115200,
                files=[(0x10000, image.name, image)],
            )
            payload = output.read_bytes()
            self.assertEqual(payload[:4], b"H2FB")
            self.assertEqual(struct.unpack("<HHIII", payload[4:20]), (1, 1, 1, 115200, 1))

    def test_rejects_overlapping_factory_members(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first.bin"
            second = root / "second.bin"
            first.write_bytes(b"1234")
            second.write_bytes(b"5678")
            with self.assertRaisesRegex(ValueError, "overlapping"):
                write_factory_bundle(
                    root / "loader.h2fb",
                    driver=1,
                    board="devkit",
                    target="esp32s3",
                    baud=115200,
                    files=[(0, first.name, first), (0, second.name, second)],
                )


if __name__ == "__main__":
    unittest.main()
