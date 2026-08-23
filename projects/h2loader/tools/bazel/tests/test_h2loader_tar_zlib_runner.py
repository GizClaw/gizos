from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zlib
from io import BytesIO


ROOT = Path(__file__).resolve().parents[5]
RUNNER = ROOT / "projects" / "h2loader" / "tools" / "bazel" / "h2loader_tar_zlib_runner.py"
os.environ["PYTHONPATH"] = str(ROOT)


class H2LoaderTarZlibRunnerTest(unittest.TestCase):
    def test_packages_esp_loader_recovery_from_symlinked_flash_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "firmware/app.bin"
            flash_root = root / "firmware/flash-files"
            actual = root / "bazel-output/bootloader.bin"
            logical = flash_root / "bootloader/bootloader.bin"
            metadata_input = root / "firmware/flasher_args.json"
            app.parent.mkdir(parents=True)
            actual.parent.mkdir(parents=True)
            logical.parent.mkdir(parents=True)
            app.write_bytes(b"application")
            actual.write_bytes(b"boot")
            logical.symlink_to(actual)
            metadata_input.write_text(
                json.dumps({"flash_files": {"0x0": "bootloader/bootloader.bin"}}),
                encoding="utf-8",
            )
            package = root / "out/loader.update.tar.zlib"
            metadata = root / "out/loader.firmware.json"
            recovery = root / "out/loader.recovery.h2fb"
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--source-root",
                    str(root),
                    "--app-image",
                    str(app),
                    "--app-path",
                    "app/esp/app.bin",
                    "--entry",
                    "entry",
                    "--platform",
                    "esp",
                    "--board",
                    "board",
                    "--image",
                    "h2loader",
                    "--role",
                    "h2loader",
                    "--target",
                    "esp32s3",
                    "--version",
                    "1.2.3",
                    "--package-output",
                    str(package),
                    "--metadata-output",
                    str(metadata),
                    "--recovery",
                    str(recovery),
                    "--esp-flash-root",
                    str(flash_root),
                    "--esp-flash-metadata",
                    str(metadata_input),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(recovery.is_file())

    def test_packages_symlinked_bazel_data_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "firmware/app.bin"
            actual_data = root / "bazel-source/startup.mp4"
            logical_data = root / "project/data/media/startup.mp4"
            app.parent.mkdir(parents=True)
            actual_data.parent.mkdir(parents=True)
            logical_data.parent.mkdir(parents=True)
            app.write_bytes(b"application")
            actual_data.write_bytes(b"video")
            logical_data.symlink_to(actual_data)
            package = root / "out/example.update.tar.zlib"
            metadata = root / "out/example.firmware.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--source-root",
                    str(root),
                    "--app-image",
                    str(app),
                    "--app-path",
                    "app/bk/app_ab_crc.rbl",
                    "--entry",
                    "entry",
                    "--platform",
                    "bk7258",
                    "--board",
                    "board",
                    "--image",
                    "example",
                    "--role",
                    "app",
                    "--target",
                    "bk7258",
                    "--version",
                    "1.2.3",
                    "--package-output",
                    str(package),
                    "--metadata-output",
                    str(metadata),
                    "--package-data-root",
                    "project/data",
                    "--package-data-file",
                    "project/data/media/startup.mp4",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with tarfile.open(fileobj=BytesIO(zlib.decompress(package.read_bytes()))) as archive:
                self.assertEqual(
                    archive.getnames(),
                    ["manifest", "checksum", "data/media/startup.mp4", "app/bk/app_ab_crc.rbl"],
                )

    def test_packages_platform_image_data_and_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "firmware/app.bin"
            data = root / "project/data/config.json"
            native = root / "firmware/firmware.elf"
            app.parent.mkdir(parents=True)
            data.parent.mkdir(parents=True)
            app.write_bytes(b"application")
            data.write_text("{}\n", encoding="utf-8")
            native.write_bytes(b"elf")
            package = root / "out/example.update.tar.zlib"
            metadata = root / "out/example.firmware.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--source-root",
                    str(root),
                    "--app-image",
                    str(app),
                    "--app-path",
                    "app/esp/app.bin",
                    "--entry",
                    "projects/example/targets/h2loader_tar_zlib/example/board",
                    "--platform",
                    "esp",
                    "--board",
                    "board",
                    "--image",
                    "example",
                    "--role",
                    "app",
                    "--target",
                    "esp32s3",
                    "--version",
                    "1.2.3",
                    "--package-output",
                    str(package),
                    "--metadata-output",
                    str(metadata),
                    "--package-data-root",
                    "project/data",
                    "--package-data-file",
                    "project/data/config.json",
                    "--native-artifact",
                    f"firmware.elf={native}",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with tarfile.open(fileobj=BytesIO(zlib.decompress(package.read_bytes()))) as archive:
                self.assertEqual(
                    archive.getnames(),
                    ["manifest", "checksum", "data/config.json", "app/esp/app.bin"],
                )
            release = json.loads(metadata.read_text(encoding="utf-8"))
            self.assertEqual(release["entry"], "projects/example/targets/h2loader_tar_zlib/example/board")
            self.assertEqual(release["assets"][0]["operation"], "managed-install")
            self.assertEqual(release["native_artifacts"][0]["name"], "firmware.elf")

    def test_rejects_data_outside_declared_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "app.bin"
            outside = root / "outside.txt"
            app.write_bytes(b"application")
            outside.write_text("outside", encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--source-root",
                    str(root),
                    "--app-image",
                    str(app),
                    "--app-path",
                    "app/bk/app_ab_crc.rbl",
                    "--entry",
                    "entry",
                    "--platform",
                    "bk7258",
                    "--board",
                    "board",
                    "--image",
                    "example",
                    "--role",
                    "app",
                    "--target",
                    "bk7258",
                    "--version",
                    "1.2.3",
                    "--package-output",
                    str(root / "out.update.tar.zlib"),
                    "--metadata-output",
                    str(root / "out.firmware.json"),
                    "--package-data-root",
                    "data",
                    "--package-data-file",
                    "outside.txt",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("escapes declared root", result.stderr)

    def test_rejects_data_path_traversal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "app.bin"
            outside = root / "outside.txt"
            app.write_bytes(b"application")
            outside.write_text("outside", encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--source-root",
                    str(root),
                    "--app-image",
                    str(app),
                    "--app-path",
                    "app/bk/app_ab_crc.rbl",
                    "--entry",
                    "entry",
                    "--platform",
                    "bk7258",
                    "--board",
                    "board",
                    "--image",
                    "example",
                    "--role",
                    "app",
                    "--target",
                    "bk7258",
                    "--version",
                    "1.2.3",
                    "--package-output",
                    str(root / "out.update.tar.zlib"),
                    "--metadata-output",
                    str(root / "out.firmware.json"),
                    "--package-data-root",
                    "data",
                    "--package-data-file",
                    "data/../outside.txt",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("escapes declared root", result.stderr)

    def test_packages_bk_image_and_records_loader_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "firmware/app_ab_crc.rbl"
            recovery = root / "firmware/loader.recovery.h2fb"
            app.parent.mkdir(parents=True)
            app.write_bytes(b"bk application")
            recovery.write_bytes(b"recovery")
            package = root / "out/loader.update.tar.zlib"
            metadata = root / "out/loader.firmware.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--source-root",
                    str(root),
                    "--app-image",
                    str(app),
                    "--app-path",
                    "app/bk/app_ab_crc.rbl",
                    "--entry",
                    "projects/h2loader/targets/h2loader_tar_zlib/loader/board",
                    "--platform",
                    "bk7258",
                    "--board",
                    "board",
                    "--image",
                    "h2loader",
                    "--role",
                    "h2loader",
                    "--target",
                    "bk7258",
                    "--version",
                    "1.2.3",
                    "--package-output",
                    str(package),
                    "--metadata-output",
                    str(metadata),
                    "--recovery",
                    str(recovery),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with tarfile.open(fileobj=BytesIO(zlib.decompress(package.read_bytes()))) as archive:
                self.assertEqual(
                    archive.getnames(),
                    ["manifest", "checksum", "app/bk/app_ab_crc.rbl"],
                )
            release = json.loads(metadata.read_text(encoding="utf-8"))
            self.assertEqual(
                [asset["operation"] for asset in release["assets"]],
                ["managed-install", "recovery"],
            )


if __name__ == "__main__":
    unittest.main()
