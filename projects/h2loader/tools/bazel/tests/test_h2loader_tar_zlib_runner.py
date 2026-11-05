from __future__ import annotations

import json
import hashlib
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


def open_data_tar(package: Path):
    with tarfile.open(package, mode="r:") as outer:
        if outer.getnames() != ["manifest", "data.tar.zlib", "app.bin.zlib"]:
            raise AssertionError("unexpected format-2 outer members")
        data = outer.extractfile("data.tar.zlib").read()
    return tarfile.open(fileobj=BytesIO(zlib.decompress(data)), mode="r:")


class H2LoaderTarZlibRunnerTest(unittest.TestCase):
    def test_format2_output_and_release_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "app.bin"
            app.write_bytes(b"firmware")
            data = root / "data/test.txt"
            data.parent.mkdir()
            data.write_bytes(b"data")
            package = root / "new.update.tar"
            metadata = root / "new.firmware.json"
            result = subprocess.run([
                sys.executable, str(RUNNER), "--source-root", str(root),
                "--app-image", str(app), "--app-path", "app/esp/app.bin",
                "--entry", "entry", "--platform", "esp", "--board", "fixture",
                "--image", "main", "--role", "app", "--target", "esp32s3", "--version", "1",
                "--package-output", str(package), "--metadata-output", str(metadata),
                "--package-data-root", "data", "--package-data-file", "data/test.txt",
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            release = json.loads(metadata.read_text())
            self.assertEqual(release["package_format"], 2)
            self.assertEqual(release["package_manifest"]["format"], 2)
            self.assertEqual(release["assets"][0]["release_suffix"], ".update.tar")
            self.assertEqual(release["assets"][0]["sha256"], hashlib.sha256(package.read_bytes()).hexdigest())
            with tarfile.open(package, "r:") as outer:
                self.assertEqual(outer.getnames(), ["manifest", "data.tar.zlib", "app.bin.zlib"])

    def test_fixed_legacy_output_and_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "app.bin"
            app.write_bytes(b"firmware")
            package, metadata = root / "old.update.tar.zlib", root / "old.firmware.json"
            result = subprocess.run([
                sys.executable, str(RUNNER), "--source-root", str(root),
                "--app-image", str(app), "--app-path", "app/esp/app.bin",
                "--entry", "old", "--platform", "esp", "--board", "fixture",
                "--image", "main", "--role", "app", "--target", "esp32s3", "--version", "1",
                "--package-format", "1", "--package-output", str(package), "--metadata-output", str(metadata),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            release = json.loads(metadata.read_text())
            self.assertEqual(release["package_format"], 1)
            self.assertEqual(release["package_manifest"]["format"], 1)
            self.assertEqual(release["assets"][0]["release_suffix"], ".update.tar.zlib")
            with tarfile.open(fileobj=BytesIO(zlib.decompress(package.read_bytes())), mode="r:") as old:
                self.assertEqual(old.extractfile("app/esp/app.bin").read(), app.read_bytes())

    def test_packages_br35_ufw_without_changing_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = root / "update.ufw"
            payload.write_bytes(b"BR35-test-UFW\x00\xff")
            package = root / "fixture.update.tar"
            metadata = root / "fixture.firmware.json"
            result = subprocess.run([
                sys.executable, str(RUNNER), "--source-root", str(root),
                "--app-image", str(payload), "--app-path", "app/jieli/update.ufw",
                "--entry", "projects/test/targets/h2loader_tar_zlib/fixture/ac707n_chip",
                "--platform", "jieli", "--board", "ac707n_chip", "--image", "fixture",
                "--role", "app", "--target", "ac707n", "--version", "1.0.0",
                "--package-output", str(package), "--metadata-output", str(metadata),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            with tarfile.open(package, mode="r:") as tar:
                self.assertEqual(zlib.decompress(tar.extractfile("app.bin.zlib").read()), payload.read_bytes())
            self.assertEqual(json.loads(metadata.read_text())["platform"], "jieli")

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
            package = root / "out/loader.update.tar"
            metadata = root / "out/loader.firmware.json"
            recovery = root / "out/loader.recovery.h2fb"
            factory_input = root / "firmware/combined_factory.bin"
            factory_output = root / "out/loader.combined_factory.bin"
            factory_input.write_bytes(b"combined factory")
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
                    "--factory-image",
                    str(factory_input),
                    "--factory-output",
                    str(factory_output),
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
            self.assertEqual(factory_output.read_bytes(), b"combined factory")
            release = json.loads(metadata.read_text(encoding="utf-8"))
            factory_asset = next(
                asset
                for asset in release["assets"]
                if asset["operation"] == "factory-flash"
            )
            self.assertEqual(factory_asset["name"], "loader.combined_factory.bin")
            self.assertEqual(factory_asset["release_suffix"], ".combined_factory.bin")
            self.assertEqual(factory_asset["flash_offset"], 0)

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
            package = root / "out/example.update.tar"
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
            with open_data_tar(package) as archive:
                self.assertEqual(
                    archive.getnames(),
                    ["data/media/startup.mp4"],
                )

    def test_packages_generated_data_under_its_repository_relative_name(self):
        """A rule may produce package data instead of a checked-in copy."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "firmware/app.bin"
            generated = root / "bazel-out/bin/project/data/media/startup.mp4"
            app.parent.mkdir(parents=True)
            generated.parent.mkdir(parents=True)
            app.write_bytes(b"application")
            generated.write_bytes(b"video")
            package = root / "out/example.update.tar"
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
                    str(root / "out/example.firmware.json"),
                    "--package-data-root",
                    "project/data",
                    "--package-data-file",
                    "project/data/media/startup.mp4",
                    "--package-data-source",
                    "bazel-out/bin/project/data/media/startup.mp4",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with open_data_tar(package) as archive:
                self.assertEqual(
                    archive.getnames(),
                    ["data/media/startup.mp4"],
                )
                member = archive.extractfile("data/media/startup.mp4")
                self.assertIsNotNone(member)
                self.assertEqual(member.read(), b"video")

    def test_rejects_package_data_source_without_a_file(self):
        """Losing the file/source correspondence must fail, not package nothing."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "firmware/app.bin"
            generated = root / "bazel-out/bin/project/data/media/startup.mp4"
            app.parent.mkdir(parents=True)
            generated.parent.mkdir(parents=True)
            app.write_bytes(b"application")
            generated.write_bytes(b"video")
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
                    str(root / "out/example.update.tar"),
                    "--metadata-output",
                    str(root / "out/example.firmware.json"),
                    "--package-data-root",
                    "project/data",
                    "--package-data-source",
                    "bazel-out/bin/project/data/media/startup.mp4",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("one source per file", result.stderr)

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
            package = root / "out/example.update.tar"
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
            with open_data_tar(package) as archive:
                self.assertEqual(
                    archive.getnames(),
                    ["data/config.json"],
                )
            release = json.loads(metadata.read_text(encoding="utf-8"))
            self.assertEqual(release["entry"], "projects/example/targets/h2loader_tar_zlib/example/board")
            self.assertEqual(release["assets"][0]["operation"], "managed-install")
            self.assertEqual(release["assets"][0]["release_suffix"], ".update.tar")
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
                    str(root / "out.update.tar"),
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
                    str(root / "out.update.tar"),
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

    def test_rejects_unresolved_git_lfs_package_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "app.bin"
            data = root / "data/images.pixa"
            data.parent.mkdir(parents=True)
            app.write_bytes(b"application")
            data.write_bytes(
                b"version https://git-lfs.github.com/spec/v1\r\n"
                b"oid sha256:a12b33d85e0d6a25a2458352d4328c5826cde92a5818c9899c0454fd58d8baf0\r\n"
                b"size 28588\r\n"
            )
            package = root / "out.update.tar"
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
                    str(root / "out.firmware.json"),
                    "--package-data-root",
                    "data",
                    "--package-data-file",
                    "data/images.pixa",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unresolved Git LFS pointer", result.stderr)
            self.assertFalse(package.exists())

    def test_packages_bk_image_and_records_loader_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "firmware/app_ab_crc.rbl"
            recovery = root / "firmware/loader.recovery.h2fb"
            app.parent.mkdir(parents=True)
            app.write_bytes(b"bk application")
            recovery.write_bytes(b"recovery")
            package = root / "out/loader.update.tar"
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
            with open_data_tar(package) as archive:
                self.assertEqual(
                    archive.getnames(),
                    [],
                )
            release = json.loads(metadata.read_text(encoding="utf-8"))
            self.assertEqual(
                [asset["operation"] for asset in release["assets"]],
                ["managed-install", "recovery"],
            )
            self.assertEqual(
                [asset["release_suffix"] for asset in release["assets"]],
                [".update.tar", ".recovery.h2fb"],
            )


@unittest.skipIf(os.name == "nt", "Flash AES packaging requires POSIX process protection")
class EncryptedRunnerTest(unittest.TestCase):
    def arguments(self, root: Path, role="app"):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner
        from projects.h2loader.tools.bazel.tests import bk_package_fixture as fixture

        native = root / "native"
        native.mkdir()
        fixture.write_outputs(role, native / "managed.rbl", native / "all-app.bin", native / "metadata", native / "config.json")
        (root / "public-test.key").write_bytes(bytes(range(32)))
        args = runner.parse_arguments([
            "--source-root", str(root), "--app-image", str(native / "managed.rbl"),
            "--app-path", "app/bk/app_ab_crc.rbl", "--entry", "source/entry",
            "--platform", "bk7258", "--board", "fixture", "--image", "mfg" if role == "h2loader" else "app",
            "--role", role, "--target", "bk7258", "--version", "1.2.3",
            "--package-output", str(root / "out/package.update.tar"),
            "--metadata-output", str(root / "out/package.firmware.json"),
            "--flash-aes-key", str(root / "public-test.key"),
            "--partition-metadata", str(native / "metadata"),
            "--encrypted-image-output", str(root / "out/image.rbl"),
            "--encryption-receipt", str(root / "out/receipt.json"),
        ])
        args.native_artifact = [("app_ab_crc.rbl", Path(args.encrypted_image_output))]
        if role == "h2loader":
            args.recovery = str(root / "out/recovery.h2fb")
            args.bk_recovery_image = str(native / "all-app.bin")
            args.bk_recovery_config = str(native / "config.json")
            args.encrypted_recovery_output = str(root / "out/recovery.bin")
            args.native_artifact.append(("all-app.bin", Path(args.encrypted_recovery_output)))
        return args

    def assert_no_publication(self, root):
        self.assertEqual(list((root / "out").rglob("*")) if (root / "out").exists() else [], [])

    def test_incomplete_or_empty_encryption_arguments_cannot_publish_plaintext(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner

        for field in runner._ENCRYPTION_FIELDS:
            for value in (None, ""):
                with self.subTest(field=field, value=value), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    args = self.arguments(root, "h2loader")
                    setattr(args, field, value)
                    with self.assertRaises(ValueError):
                        runner.run(args)
                    self.assert_no_publication(root)

    def test_wrong_identity_and_recovery_arguments_are_refused(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner

        for field, value in (
            ("target", "esp32s3"), ("platform", "esp"), ("app_path", "app/esp/app.bin"),
            ("factory_image", "factory.bin"), ("recovery", None), ("bk_recovery_image", None),
            ("bk_recovery_config", None), ("package_data_file", ["data/config"]),
        ):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                args = self.arguments(root, "h2loader")
                setattr(args, field, value)
                with self.assertRaises(ValueError):
                    runner.run(args)
                self.assert_no_publication(root)

    def test_invalid_key_removes_stale_outputs(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner

        for role in ("app", "h2loader"):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                args = self.arguments(root, role)
                (root / "out").mkdir()
                for field in runner._OUTPUT_FIELDS:
                    if getattr(args, field):
                        Path(getattr(args, field)).write_bytes(b"stale output")
                Path(args.flash_aes_key).write_bytes(b"short key")
                with self.assertRaisesRegex(ValueError, "KEY_LENGTH"):
                    runner.run(args)
                self.assert_no_publication(root)

    def test_failure_in_common_packaging_or_metadata_removes_all_outputs(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner
        from unittest.mock import patch

        for helper in ("publish_managed_package", "publish_metadata", "publish_encryption_receipt"):
            with self.subTest(helper=helper), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                args = self.arguments(root, "h2loader")
                with patch.object(runner, helper, side_effect=OSError("simulated publication failure")):
                    with self.assertRaises(OSError):
                        runner.run(args)
                self.assert_no_publication(root)

    def test_failed_final_move_removes_already_published_files(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.arguments(root, "h2loader")
            replace = Path.replace

            def fail_metadata(source, destination):
                if destination == Path(args.metadata_output):
                    raise OSError("simulated final publication failure")
                return replace(source, destination)

            with patch.object(Path, "replace", fail_metadata):
                with self.assertRaises(OSError):
                    runner.run(args)
            self.assert_no_publication(root)

    def test_output_collision_cannot_delete_or_overwrite_native_input(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.arguments(root)
            source = Path(args.app_image).read_bytes()
            args.package_output = args.app_image
            with self.assertRaises(ValueError):
                runner.run(args)
            self.assertEqual(Path(args.app_image).read_bytes(), source)
            self.assert_no_publication(root)

    def test_runtime_errors_do_not_log_values_or_tracebacks(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner
        from unittest.mock import patch
        from contextlib import redirect_stderr, redirect_stdout
        import io

        out, err = io.StringIO(), io.StringIO()
        with patch.object(runner, "parse_arguments"), patch.object(runner, "run", side_effect=ValueError("sensitive contents")):
            with redirect_stdout(out), redirect_stderr(err):
                self.assertEqual(runner.main(["--flash-aes-key", "key-file"]), 1)
        self.assertEqual(out.getvalue(), "")
        self.assertIn("encrypted package refused", err.getvalue())
        self.assertNotIn("sensitive contents", err.getvalue())
        self.assertNotIn("Traceback", err.getvalue())

    def test_bootloader_receipt_identity_reaches_only_mapped_evidence(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.arguments(root, "h2loader")
            document = json.dumps({
                "schema": 1, "contract": "mapped_ab_v1",
                "native_image_sha256": hashlib.sha256(Path(args.bk_recovery_image).read_bytes()).hexdigest(),
            }).encode()
            (Path(args.partition_metadata) / "bootloader.json").write_bytes(document)
            runner.run(args)
            receipt = json.loads(Path(args.encryption_receipt).read_text())
            self.assertEqual(receipt["bootloader_contract"], "mapped_ab_v1")
            self.assertEqual(receipt["bootloader_receipt_sha256"], hashlib.sha256(document).hexdigest())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.arguments(root, "h2loader")
            runner.run(args)
            receipt = json.loads(Path(args.encryption_receipt).read_text())
            self.assertNotIn("bootloader_contract", receipt)
            self.assertNotIn("bootloader_receipt_sha256", receipt)

    def test_duplicate_or_abbreviated_encryption_flags_are_rejected(self):
        from projects.h2loader.tools.bazel import h2loader_tar_zlib_runner as runner
        from contextlib import redirect_stderr
        import io

        # Use the real CLI to exercise parser rejection before any publication.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.arguments(root)
            command = [
                "--source-root", str(root), "--app-image", args.app_image,
                "--app-path", args.app_path, "--entry", "entry", "--platform", "bk7258",
                "--board", "fixture", "--image", "app", "--role", "app", "--target", "bk7258",
                "--version", "1", "--package-output", args.package_output, "--metadata-output", args.metadata_output,
            ]
            for extra in (["--flash-aes-key=key", "--flash-aes-key="], ["--flash-aes-ke=key"]):
                with self.subTest(extra=extra), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    runner.parse_arguments(command + extra)
                self.assert_no_publication(root)

    def test_recovery_bundle_and_receipt_use_the_original_snapshots(self):
        from projects.h2loader.tools.bazel import firmware_output, h2loader_tar_zlib_runner as runner
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.arguments(root, "h2loader")
            source_hash = hashlib.sha256(Path(args.app_image).read_bytes()).hexdigest()
            metadata_hash = hashlib.sha256((Path(args.partition_metadata) / "partitions.json").read_bytes()).hexdigest()
            read_key = firmware_output.read_flash_aes_key

            def change_inputs(path):
                for source in (Path(args.app_image), Path(args.bk_recovery_image), Path(args.bk_recovery_config), Path(args.partition_metadata) / "partitions.json"):
                    source.write_bytes(b"changed after snapshot")
                return read_key(path)

            with patch.object(firmware_output, "read_flash_aes_key", side_effect=change_inputs):
                runner.run(args)
            receipt = json.loads(Path(args.encryption_receipt).read_text())
            self.assertEqual(receipt["source_image_sha256"], source_hash)
            self.assertEqual(receipt["partition_metadata_sha256"], metadata_hash)
            metadata = json.loads(Path(args.metadata_output).read_text())
            self.assertEqual(metadata["entry"], "source/entry")
            self.assertEqual(metadata["image"], "mfg")
            self.assertTrue(Path(args.recovery).exists())


if __name__ == "__main__":
    unittest.main()
