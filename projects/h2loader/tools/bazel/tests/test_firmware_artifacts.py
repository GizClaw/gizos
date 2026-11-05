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

from projects.h2loader.tools.bazel.firmware_artifacts import BundleEntry, read_managed_package, write_factory_bundle, write_package


def fixture_path(name: str) -> Path:
    if "TEST_SRCDIR" not in os.environ:
        return Path(__file__).parent / "fixtures" / name
    from python.runfiles import Runfiles
    return Path(Runfiles.Create().Rlocation("gizos/projects/h2loader/tools/bazel/tests/fixtures/" + name))


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


@unittest.skipIf(os.name == "nt", "Flash AES packaging requires POSIX process protection")
class FlashAesEncryptionTests(unittest.TestCase):
    KEY = bytes(range(32))

    def encrypt(self, root: Path, role: str, *, key: bytes | None = None, metadata_role: str | None = None, offset: str = "0x0", mutate=None):
        from projects.h2loader.tools.bazel import firmware_output
        from projects.h2loader.tools.bazel.tests import bk_package_fixture as fixture

        native = root / "native"
        native.mkdir(exist_ok=True)
        managed, recovery, config = native / "managed.rbl", native / "recovery.bin", native / "config.json"
        metadata = native / "metadata"
        fixture.write_outputs(metadata_role or role, native / "unused.rbl", recovery, metadata, config)
        fixture.write_outputs(role, managed, recovery, native / "own-metadata", config)
        config.write_text(json.dumps({"offset": offset, "baud": 115200}))
        key_file = root / "key"
        key_file.write_bytes(self.KEY if key is None else key)
        is_loader = role == "h2loader"
        if mutate is not None:
            mutate(managed, recovery, metadata, config)
        return firmware_output.encrypt_bk_images(
            key_file=key_file,
            partition_metadata=metadata,
            app_image=managed,
            app_output=root / "encrypted.rbl",
            recovery_image=recovery if is_loader else None,
            recovery_config=config if is_loader else None,
            recovery_output=root / "encrypted.bin" if is_loader else None,
        )

    def test_linux_dump_protection_fails_closed(self):
        from projects.h2loader.tools.bazel.firmware_output import _protect_secret_process
        from unittest.mock import patch

        with patch("sys.platform", "linux"), patch("resource.setrlimit"), patch("ctypes.CDLL") as library:
            for results in ((-1,), (0, 1)):
                library.return_value.prctl.side_effect = results
                with self.subTest(results=results), self.assertRaisesRegex(ValueError, "DUMP_PROTECTION_FAILED"):
                    _protect_secret_process()
            library.return_value.prctl.side_effect = (0, 0)
            _protect_secret_process()

    @unittest.skipUnless(sys.platform == "linux", "Linux process dumpability")
    def test_linux_process_is_not_dumpable(self):
        script = (
            "import sys; sys.path[:] = " + repr(sys.path) + "; "
            "from projects.h2loader.tools.bazel.firmware_output import _protect_secret_process; "
            "_protect_secret_process(); import ctypes; "
            "assert ctypes.CDLL(None).prctl(3, 0, 0, 0, 0) == 0"
        )
        subprocess.run([sys.executable, "-c", script], check=True, capture_output=True, timeout=10)

    def test_key_file_is_exactly_32_raw_bytes(self):
        from projects.h2loader.tools.bazel.firmware_output import read_flash_aes_key

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "key"
            path.write_bytes(self.KEY)
            self.assertEqual(read_flash_aes_key(path), bytearray(self.KEY))
            for value in (b"", self.KEY[:31], self.KEY + b"\n", self.KEY.hex().encode()):
                with self.subTest(length=len(value)):
                    path.write_bytes(value)
                    with self.assertRaisesRegex(ValueError, "^KEY_LENGTH$"):
                        read_flash_aes_key(path)

    def test_key_file_must_be_regular(self):
        from projects.h2loader.tools.bazel.firmware_output import read_flash_aes_key

        with tempfile.TemporaryDirectory() as directory:
            fifo = Path(directory) / "key"
            os.mkfifo(fifo)
            with self.assertRaisesRegex(ValueError, "KEY_REQUIRES_REGULAR_FILE"):
                read_flash_aes_key(fifo)

    def test_invalid_key_publishes_nothing(self):
        for role in ("app", "h2loader"):
            for value in (b"", self.KEY[:31], self.KEY + b"\n"):
                with self.subTest(role=role, length=len(value)), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    with self.assertRaisesRegex(ValueError, "^KEY_LENGTH$"):
                        self.encrypt(root, role, key=value)
                    self.assertFalse((root / "encrypted.rbl").exists())
                    self.assertFalse((root / "encrypted.bin").exists())

    def test_invalid_layout_is_refused_before_the_key_is_read(self):
        from unittest.mock import patch

        for kwargs, message in (
            ({"metadata_role": "app"}, "APP_VECTOR"),
            ({"offset": "0x1000"}, "RECOVERY_REQUIRES_ZERO_OFFSET"),
        ):
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with patch("projects.h2loader.tools.bazel.firmware_output.read_flash_aes_key") as read_key:
                    with self.assertRaisesRegex(ValueError, message):
                        self.encrypt(root, "h2loader", **kwargs)
                    read_key.assert_not_called()
                self.assertFalse((root / "encrypted.rbl").exists())

    def test_unprotected_process_never_reads_the_key(self):
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as directory:
            with (
                patch("projects.h2loader.tools.bazel.firmware_output._protect_secret_process", side_effect=ValueError("DUMP_PROTECTION_FAILED")),
                patch("projects.h2loader.tools.bazel.firmware_output.read_flash_aes_key") as read_key,
                self.assertRaisesRegex(ValueError, "DUMP_PROTECTION_FAILED"),
            ):
                self.encrypt(Path(directory), "app")
            read_key.assert_not_called()

    def test_inputs_are_bounded_before_reading_the_key(self):
        from unittest.mock import patch

        for index, limit in ((0, 16 * 1024 * 1024), (1, 16 * 1024 * 1024), (2, 1024 * 1024), (3, 1024 * 1024)):
            def oversize(*paths):
                path = paths[index] / "partitions.json" if index == 2 else paths[index]
                with path.open("wb") as stream:
                    stream.truncate(limit + 1)

            with self.subTest(input=index), tempfile.TemporaryDirectory() as directory:
                with patch("projects.h2loader.tools.bazel.firmware_output.read_flash_aes_key") as read_key:
                    with self.assertRaisesRegex(ValueError, "ENCRYPTION_INPUT_SIZE"):
                        self.encrypt(Path(directory), "h2loader", mutate=oversize)
                    read_key.assert_not_called()

    def test_keyless_inputs_and_receipt_hashes_use_one_snapshot(self):
        from projects.h2loader.tools.bazel import firmware_output
        from projects.h2loader.tools.bazel.tests import bk_package_fixture as fixture
        from tools.bazel.bk7258_flash_aes import encrypt_app, encrypt_recovery
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original_app = fixture.managed_image(fixture.LOADER_PHYSICAL_BASE)
            original_recovery = fixture.recovery_image()
            original_metadata = json.dumps(fixture.partition_table(fixture.LOADER_PHYSICAL_BASE)).encode()

            def read_key(path):
                # Change all original inputs at the moment the secret is read.
                # Encryption and later receipts must describe the earlier bytes.
                for name in ("managed.rbl", "recovery.bin", "metadata/partitions.json", "config.json"):
                    (root / "native" / name).write_bytes(b"replaced after snapshot")
                return bytearray(self.KEY)

            with patch.object(firmware_output, "read_flash_aes_key", side_effect=read_key):
                result = self.encrypt(root, "h2loader")
            frozen = root / "expected-metadata"
            frozen.mkdir()
            (frozen / "partitions.json").write_bytes(original_metadata)
            self.assertEqual((root / "encrypted.rbl").read_bytes(), encrypt_app(original_app, frozen, self.KEY))
            self.assertEqual((root / "encrypted.bin").read_bytes(), encrypt_recovery(original_recovery, frozen, self.KEY))
            self.assertEqual(result.source_image_sha256, hashlib.sha256(original_app).hexdigest())
            self.assertEqual(result.partition_metadata_sha256, hashlib.sha256(original_metadata).hexdigest())
            self.assertEqual(result.recovery_config, {"offset": "0x0", "baud": 115200})

    def test_optional_bootloader_receipt_is_recorded_or_refused_before_the_key(self):
        from unittest.mock import patch

        def receipt(**fields):
            def write(managed, recovery, metadata, config):
                document = {"schema": 1, "contract": "mapped_ab_v1", "native_image_sha256": hashlib.sha256(recovery.read_bytes()).hexdigest()}
                (metadata / "bootloader.json").write_text(json.dumps(document | fields))
            return write

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.encrypt(root, "h2loader", mutate=receipt())
            self.assertEqual(result.bootloader_contract, "mapped_ab_v1")
            self.assertEqual(result.bootloader_receipt_sha256, hashlib.sha256((root / "native/metadata/bootloader.json").read_bytes()).hexdigest())
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(self.encrypt(Path(directory), "h2loader").bootloader_contract)
        for fields, message in (
            ({"schema": True}, "^BOOTLOADER_RECEIPT$"),
            ({"schema": 2}, "^BOOTLOADER_RECEIPT$"),
            ({"contract": "other"}, "^BOOTLOADER_RECEIPT$"),
            ({"native_image_sha256": "0" * 64}, "^BOOTLOADER_RECEIPT_IMAGE$"),
        ):
            with self.subTest(fields=fields), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with patch("projects.h2loader.tools.bazel.firmware_output.read_flash_aes_key") as read_key:
                    with self.assertRaisesRegex(ValueError, message):
                        self.encrypt(root, "h2loader", mutate=receipt(**fields))
                    read_key.assert_not_called()
                self.assertFalse((root / "encrypted.rbl").exists())

    def test_corrupt_recovery_publishes_nothing_and_clears_the_key(self):
        from unittest.mock import patch

        def corrupt(managed, recovery, metadata, config):
            data = bytearray(recovery.read_bytes())
            data[0] ^= 1
            recovery.write_bytes(data)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            key = bytearray(self.KEY)
            with patch("projects.h2loader.tools.bazel.firmware_output.read_flash_aes_key", return_value=key):
                with self.assertRaisesRegex(ValueError, "SOURCE_CRC"):
                    self.encrypt(root, "h2loader", mutate=corrupt)
            self.assertEqual(key, bytearray(32))
            self.assertFalse((root / "encrypted.rbl").exists())
            self.assertFalse((root / "encrypted.bin").exists())

    def test_corrupt_app_is_refused_before_reading_the_key(self):
        from projects.h2loader.tools.bazel.tests import bk_package_fixture as fixture
        from unittest.mock import patch

        for repair_crc, message in ((False, "SOURCE_CRC"), (True, "RBL_DIGEST")):
            def corrupt(managed, recovery, metadata, config):
                data = bytearray(managed.read_bytes())
                data[32 * 34] ^= 1
                if repair_crc:
                    plain = b"".join(data[i:i + 32] for i in range(0, len(data), 34))
                    data = fixture._stored(plain)
                managed.write_bytes(data)

            with self.subTest(repair_crc=repair_crc), tempfile.TemporaryDirectory() as directory:
                with patch("projects.h2loader.tools.bazel.firmware_output.read_flash_aes_key") as read_key:
                    with self.assertRaisesRegex(ValueError, message):
                        self.encrypt(Path(directory), "app", mutate=corrupt)
                    read_key.assert_not_called()

    def test_each_role_encrypts_for_its_own_window(self):
        for role, logical_base in (("app", 0x240000), ("h2loader", 0x1000)):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.assertEqual(self.encrypt(root, role).logical_base, logical_base)
                self.assertNotEqual((root / "encrypted.rbl").read_bytes(), (root / "native/managed.rbl").read_bytes())
                self.assertEqual((root / "encrypted.bin").exists(), role == "h2loader")


if __name__ == "__main__":
    unittest.main()
