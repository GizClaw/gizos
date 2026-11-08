from __future__ import annotations

from pathlib import Path
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import io
import json
import tarfile
import zlib

from tools.h2loader.firmware_artifacts import BundleEntry, read_managed_package, write_factory_bundle, write_package


def fixture_path(name: str) -> Path:
    if "TEST_SRCDIR" not in os.environ:
        return Path(__file__).parent / "fixtures" / name
    from python.runfiles import Runfiles
    return Path(Runfiles.Create().Rlocation("gizos/tools/h2loader/tests/fixtures/" + name))


class FirmwareArtifactsTest(unittest.TestCase):
    def test_conversion_reader_roundtrip_and_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)/"update"
            entries=[BundleEntry("data/a.txt",b"a"),BundleEntry("data/b.pixa",b"picture")]
            for fmt in (1,2):
                write_package(out,"app/bk/app_ab_crc.rbl",b"image",entries,role="app",board="fixture",target="bk7258",version="1",package_format=fmt)
                data=out.read_bytes()
                parsed=read_managed_package(data)
                self.assertEqual(parsed.app,b"image")
                self.assertEqual(parsed.data,entries)
                for bad in (data[:100], data+b"trailing" if fmt==1 else data+b"\x01", data[:2048] if fmt==2 else data[:-1]):
                    with self.subTest(fmt=fmt,size=len(bad)),self.assertRaises(ValueError):
                        read_managed_package(bad)
                unpacked=zlib.decompress(data) if fmt==1 else data
                tf=tarfile.open(fileobj=io.BytesIO(unpacked))
                changed=io.BytesIO()
                with tarfile.open(fileobj=changed,mode="w",format=tarfile.USTAR_FORMAT) as dst:
                    for m in tf:
                        content=tf.extractfile(m).read()
                        if m.name=="manifest": content=content.replace(b"version=1",b"version=2\nversion=1")
                        m.size=len(content);dst.addfile(m,io.BytesIO(content))
                with self.assertRaises(ValueError):
                    read_managed_package(zlib.compress(changed.getvalue()) if fmt==1 else changed.getvalue())

    def test_independent_format2_members(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "update.tar"
            options = dict(role="app", board="fixture", target="host", version="0")
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

    def test_legacy_producer_preserves_frozen_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "update.tar.zlib"
            write_package(output, "app/esp/app.bin", bytes((0xE9, 1, 2, 3, 4, 5)),
                [BundleEntry("data/a.txt", b"alpha"), BundleEntry("data/z.bin", bytes((0, 1, 2)))],
                role="app", board="fixture", target="host", version="0", package_format=1)
            self.assertEqual(output.read_bytes(), fixture_path("h2loader_format1.tar.zlib").read_bytes())

    def test_invalid_package_formats_are_rejected(self) -> None:
        for value in (True, 1.0, 0, 3):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory, self.assertRaises(ValueError):
                write_package(Path(directory) / "update", "app/esp/app.bin", b"app", [],
                    role="app", board="fixture", target="host", version="0", package_format=value)

    def test_frozen_legacy_archive_remains_readable(self) -> None:
        data = fixture_path("h2loader_format1.tar.zlib").read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), fixture_path("h2loader_format1.sha256").read_text().strip())
        with tarfile.open(fileobj=io.BytesIO(zlib.decompress(data))) as old:
            self.assertEqual(old.extractfile("app/esp/app.bin").read(), bytes((0xE9, 1, 2, 3, 4, 5)))

    def test_canonical_h2loader_package_digest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "update.tar"
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
            fixture = fixture_path("h2loader_format2.sha256").read_text(encoding="ascii").strip()
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
