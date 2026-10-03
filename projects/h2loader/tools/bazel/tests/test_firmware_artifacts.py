from __future__ import annotations

from pathlib import Path
import hashlib
import os
import struct
import tempfile
import unittest
import io
import tarfile
import zlib

from projects.h2loader.tools.bazel.firmware_artifacts import BundleEntry, write_factory_bundle, write_package


class FirmwareArtifactsTest(unittest.TestCase):
    def test_independent_format2_members(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "update.tar"
            options = dict(role="app", board="fixture", target="host", version="0", package_format=2)
            entries = [BundleEntry("data/z.bin", b"zed"), BundleEntry("data/a.txt", b"alpha")]
            write_package(output, "app/esp/app.bin", b"firmware", entries, **options)
            original = output.read_bytes()
            write_package(output, "app/esp/app.bin", b"firmware", list(reversed(entries)), **options)
            self.assertEqual(original, output.read_bytes())
            with tarfile.open(fileobj=io.BytesIO(original), mode="r:") as outer:
                self.assertEqual(outer.getnames(), ["manifest", "data.tar.zlib", "app.bin.zlib"])
                manifest = dict(line.split("=", 1) for line in outer.extractfile("manifest").read().decode().splitlines())
                app = outer.extractfile("app.bin.zlib").read()
                data = outer.extractfile("data.tar.zlib").read()
                self.assertEqual(manifest["format"], "2")
                self.assertEqual(hashlib.sha256(app).hexdigest(), manifest["app_zlib_sha256"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), manifest["data_zlib_sha256"])
                self.assertEqual(zlib.decompress(app), b"firmware")
                decoded = zlib.decompress(data)
                self.assertEqual(len(decoded), int(manifest["data_tar_size"]))
                with tarfile.open(fileobj=io.BytesIO(decoded), mode="r:") as inner:
                    self.assertEqual(inner.getnames(), ["data/a.txt", "data/z.bin"])
                    self.assertEqual(inner.extractfile("data/a.txt").read(), b"alpha")

    def test_rejects_noninteger_package_formats(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            for value in (True, False, 1.0, 2.0, "2", 0, 3):
                with self.subTest(value=value), self.assertRaisesRegex(ValueError, "package format"):
                    write_package(Path(directory) / "update", "app/esp/app.bin", b"app", [],
                        role="app", board="fixture", target="host", version="0", package_format=value)

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
