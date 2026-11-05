"""Native encryption followed by package-only update/recovery publication."""

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from python.runfiles import Runfiles
from projects.h2loader.tools.bazel import repackage
from projects.h2loader.tools.bazel.firmware_artifacts import (
    BundleEntry, FACTORY_IDENTITY_SIZE, read_managed_package, write_package,
)
from tools.bazel.tests.bk7258_encryption.fixture import fixture


class PackageConversionTest(unittest.TestCase):
    def executable(self, path):
        result = Runfiles.Create().Rlocation("gizos/" + path)
        self.assertIsNotNone(result)
        return result

    def encrypt(self, root, kind):
        source, metadata = fixture(kind)
        original = root / "original.bin"
        original.write_bytes(source)
        (root / "partitions.json").write_text(json.dumps(metadata))
        tool = self.executable("tools/bazel/bk7258_encrypt")
        result = subprocess.run(
            [tool, "--source", str(original), "--metadata", str(root), "--image", kind,
             "--key-fd", "0", "--output-dir", str(root / "native")],
            input=bytes(range(32)), capture_output=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        name = "app.flash-aes.rbl" if kind == "app" else "full.flash-aes.bin"
        return source, root / "native" / name, root / "native/validation.json"

    def test_app_formats_reuse_the_processed_image_and_preserve_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, image, receipt = self.encrypt(root, "app")
            entries = [BundleEntry("data/test.txt", b"unchanged")]
            tool = self.executable("projects/h2loader/tools/bazel/repackage")
            for fmt in (1, 2):
                original = root / ("original-" + str(fmt))
                write_package(original, "app/bk/app_ab_crc.rbl", source, entries,
                              role="app", board="fixture", target="bk7258", version="1", package_format=fmt)
                output = root / ("packaged-" + str(fmt))
                result = subprocess.run(
                    [tool, "app", "--source", str(original), "--image", str(image),
                     "--receipt", str(receipt), "--output-dir", str(output)],
                    capture_output=True, timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                archive = output / ("app.update.tar.zlib" if fmt == 1 else "app.update.tar")
                package = read_managed_package(archive.read_bytes())
                self.assertEqual(package.app, image.read_bytes())
                self.assertEqual(package.data, entries)
                self.assertEqual(package.manifest["format"], str(fmt))
                self.assertEqual(package.manifest["board"], "fixture")
                self.assertEqual(package.manifest["version"], "1")
                if fmt == 1:
                    self.assertEqual(package.app_path, "app/bk/app_ab_crc.rbl")
                self.assertEqual(output.stat().st_mode & 0o777, 0o700)
                for path in output.iterdir():
                    self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                before = archive.read_bytes()
                with redirect_stderr(io.StringIO()):
                    self.assertEqual(repackage.main([
                        "app", "--source", str(original), "--image", str(image), "--receipt", str(receipt),
                        "--output-dir", str(output),
                    ]), 1)
                self.assertEqual(archive.read_bytes(), before)
            self.assertEqual((root / "original.bin").read_bytes(), source)

    def test_recovery_wraps_native_bytes_without_encrypting_again(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, image, receipt = self.encrypt(root, "full")
            config = root / "recovery.json"
            config.write_text(json.dumps(dict(offset="0x00000000", baud=115200)))
            output = root / "package"
            args = ["recovery", "--image", str(image), "--receipt", str(receipt), "--config", str(config),
                    "--board", "fixture", "--target", "bk7258", "--output-dir", str(output)]
            with redirect_stdout(io.StringIO()):
                self.assertEqual(repackage.main(args), 0)
            bundle = (output / "recovery.h2fb").read_bytes()
            self.assertEqual(bundle[:4], b"H2FB")
            self.assertEqual(struct.unpack_from("<HHIII", bundle, 4), (1, 2, 1, 115200, 1))
            offset, start, length = struct.unpack_from("<IQQ", bundle, 20 + 2 * FACTORY_IDENTITY_SIZE)
            self.assertEqual(offset, 0)
            self.assertEqual(bundle[start:start + length], image.read_bytes())
            self.assertNotEqual(image.read_bytes(), source)
            config.write_text(json.dumps(dict(offset="0x1000", baud=115200)))
            with redirect_stderr(io.StringIO()):
                self.assertEqual(repackage.main(args[:-1] + [str(root / "bad")]), 1)
            self.assertFalse((root / "bad").exists())

    def app_inputs(self, root, target="bk7258", app_path="app/bk/app_ab_crc.rbl"):
        source = root / "source.update"
        image = root / "processed.bin"
        image.write_bytes(b"native transformed bytes")
        write_package(source, app_path, b"original image", [BundleEntry("data/a.txt", b"data")],
                      role="app", board="fixture", target=target, version="1", package_format=1)
        receipt = {
            "target": target, "image": "app", "image_size": image.stat().st_size,
            "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
            "source_image_sha256": hashlib.sha256(b"original image").hexdigest(),
        }
        (root / "validation.json").write_text(json.dumps(receipt))
        return receipt, ["app", "--source", str(source), "--image", str(image),
                         "--receipt", str(root / "validation.json"), "--output-dir", str(root / "output")]

    def test_receipt_binds_source_image_target_and_transformed_bytes(self):
        for field, value in (("image_sha256", "0" * 64), ("source_image_sha256", "0" * 64),
                             ("target", "esp32s3"), ("image", "full"), ("image_size", True)):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                receipt, args = self.app_inputs(root)
                receipt[field] = value
                (root / "validation.json").write_text(json.dumps(receipt))
                with redirect_stderr(io.StringIO()):
                    self.assertEqual(repackage.main(args), 1)
                self.assertFalse((root / "output").exists())
                self.assertFalse(list(root.glob(".h2-package-*")))

    def test_repackaging_preserves_other_platform_app_paths(self):
        for target, path in (("esp32s3", "app/esp/app.bin"), ("wl82", "app/jieli/update.ufw")):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                _, args = self.app_inputs(root, target, path)
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(repackage.main(args), 0)
                parsed = read_managed_package((root / "output/app.update.tar.zlib").read_bytes())
                self.assertEqual(parsed.app_path, path)
                self.assertEqual(parsed.manifest["target"], target)

    def test_failure_cleans_staging_without_publishing_a_partial_package(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, args = self.app_inputs(root)
            with patch.object(repackage, "write_package", side_effect=ValueError("fixture")), redirect_stderr(io.StringIO()):
                self.assertEqual(repackage.main(args), 1)
            self.assertFalse((root / "output").exists())
            self.assertFalse(list(root.glob(".h2-package-*")))


if __name__ == "__main__":
    unittest.main()
