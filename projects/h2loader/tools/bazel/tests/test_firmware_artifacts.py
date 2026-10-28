from __future__ import annotations

from pathlib import Path
import hashlib
import os
import struct
import sys
import tempfile
import unittest
import io
import json
import tarfile
import zlib

from projects.h2loader.tools.bazel.firmware_artifacts import BundleEntry, write_factory_bundle, write_package


def fixture_path(name: str) -> Path:
    if "TEST_SRCDIR" not in os.environ:
        return Path(__file__).parent / "fixtures" / name
    from python.runfiles import Runfiles
    return Path(Runfiles.Create().Rlocation("gizos/projects/h2loader/tools/bazel/tests/fixtures/" + name))


class FirmwareArtifactsTest(unittest.TestCase):
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



@unittest.skipIf(os.name == "nt", "local recovery entrypoint requires POSIX secret descriptors")
class EncryptedRecoveryTests(unittest.TestCase):
    def test_linux_dump_protection_fails_closed(self):
        from projects.h2loader.tools.bazel.firmware_output import _protect_recovery_process
        from unittest.mock import patch

        with patch("sys.platform", "linux"), patch("resource.setrlimit"), patch("ctypes.CDLL") as library:
            for results in ((-1,), (0, 1)):
                library.return_value.prctl.side_effect = results
                with self.subTest(results=results), self.assertRaisesRegex(ValueError, "DUMP_PROTECTION_FAILED"):
                    _protect_recovery_process()
            library.return_value.prctl.side_effect = (0, 0)
            _protect_recovery_process()

    @unittest.skipUnless(sys.platform == "linux", "Linux process dumpability")
    def test_linux_process_is_not_dumpable(self):
        import subprocess
        import sys

        script = (
            "import sys; sys.path[:] = " + repr(sys.path) + "; "
            "from projects.h2loader.tools.bazel.firmware_output import _protect_recovery_process; "
            "_protect_recovery_process(); import ctypes; "
            "assert ctypes.CDLL(None).prctl(3, 0, 0, 0, 0) == 0"
        )
        subprocess.run([sys.executable, "-c", script], check=True, capture_output=True, timeout=10)

    def test_secret_stream_has_a_total_deadline(self):
        from projects.h2loader.tools.bazel.firmware_output import _read_recovery_key
        import socket

        for data in (b"", bytes(16), bytes(32)):
            with self.subTest(length=len(data)):
                reader, writer = os.pipe()
                try:
                    os.write(writer, data)
                    with self.assertRaisesRegex(ValueError,"KEY_INPUT_TIMEOUT"):
                        _read_recovery_key(reader, timeout=0.02)
                finally:
                    os.close(reader)
                    os.close(writer)
        # select.select rejects valid descriptors >= FD_SETSIZE on POSIX.
        # Exercise the public descriptor contract when the host limit allows it.
        import fcntl
        import resource
        if resource.getrlimit(resource.RLIMIT_NOFILE)[0] > 1024:
            reader, writer = os.pipe()
            high_fd = None
            try:
                high_fd = fcntl.fcntl(reader, fcntl.F_DUPFD, 1024)
                os.write(writer,bytes(range(32)))
                os.close(writer)
                writer = None
                self.assertEqual(_read_recovery_key(high_fd),bytearray(range(32)))
            finally:
                os.close(reader)
                if high_fd is not None:
                    os.close(high_fd)
                if writer is not None:
                    os.close(writer)
        left, right = socket.socketpair()
        try:
            right.sendall(bytes(range(32)))
            right.shutdown(socket.SHUT_WR)
            self.assertEqual(_read_recovery_key(left.fileno()),bytearray(range(32)))
        finally:
            left.close()
            right.close()

    def test_runtime_pipe_and_atomic_publication(self):
        from projects.h2loader.tools.bazel.firmware_output import encrypted_bk_recovery_main
        from tools.bazel.bk7258_flash_aes import flash_crc
        from unittest.mock import patch
        import contextlib

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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
            source = b"".join(plain[i:i+32]+flash_crc(plain[i:i+32]) for i in range(0,len(plain),32))
            (root / "source.bin").write_bytes(source)
            (root / "config.json").write_text(json.dumps(dict(offset="0x00000000", baud=115200)))
            def invoke(secret, destination, failure=False):
                read_fd, write_fd = os.pipe()
                os.write(write_fd, secret)
                os.close(write_fd)
                capture = io.StringIO()
                try:
                    with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
                        with patch("projects.h2loader.tools.bazel.firmware_output._protect_recovery_process"):
                            if failure:
                                with patch("projects.h2loader.tools.bazel.firmware_output.publish_bk_recovery", side_effect=ValueError("SECRET_MARKER")):
                                    rc = encrypted_bk_recovery_main(args(read_fd,destination))
                            else:
                                rc = encrypted_bk_recovery_main(args(read_fd,destination))
                    self.assertNotIn("SECRET_MARKER", capture.getvalue())
                    if secret:
                        self.assertNotIn(secret.hex(), capture.getvalue())
                    return rc
                finally:
                    os.close(read_fd)
            def args(fd,dest):
                return ["--source",str(root/"source.bin"),"--metadata",str(root),"--config",str(root/"config.json"),"--board","fixture","--target","bk7258","--key-fd",str(fd),"--output-dir",str(dest)]
            output = root / "published"
            self.assertEqual(invoke(bytes(range(32)),output),0)
            self.assertEqual(set(p.name for p in output.iterdir()),{"encrypted.bin","recovery.h2fb","validation.json"})
            bundle = (output/"recovery.h2fb").read_bytes()
            self.assertEqual(bundle[:4], b"H2FB")
            self.assertEqual(struct.unpack_from("<HHIII",bundle,4),(1,2,1,115200,1))
            from projects.h2loader.tools.bazel.firmware_artifacts import FACTORY_IDENTITY_SIZE
            flash_offset, data_offset, length = struct.unpack_from("<IQQ",bundle,20+2*FACTORY_IDENTITY_SIZE)
            self.assertEqual(flash_offset,0)
            self.assertEqual(bundle[data_offset:data_offset+length],(output/"encrypted.bin").read_bytes())
            self.assertNotEqual((output/"encrypted.bin").read_bytes(),source)
            receipt=json.loads((output/"validation.json").read_text())
            self.assertEqual(receipt["encrypted_sha256"],hashlib.sha256((output/"encrypted.bin").read_bytes()).hexdigest())
            self.assertEqual(output.stat().st_mode & 0o777,0o700)
            self.assertEqual(invoke(bytes(range(32)),output),1)
            with patch("projects.h2loader.tools.bazel.firmware_output._protect_recovery_process", side_effect=ValueError("DUMP_PROTECTION_FAILED")), patch("projects.h2loader.tools.bazel.firmware_output._read_recovery_key") as read_key, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(encrypted_bk_recovery_main(args(0,root/"unprotected")),1)
                read_key.assert_not_called()
            self.assertFalse((root/"unprotected").exists())
            with patch("builtins.print", side_effect=BrokenPipeError("SECRET_MARKER")):
                self.assertEqual(invoke(bytes(range(32)),root/"closed-stdout"),0)
            self.assertEqual(set(p.name for p in (root/"closed-stdout").iterdir()),{"encrypted.bin","recovery.h2fb","validation.json"})
            with patch("builtins.print", side_effect=BrokenPipeError("SECRET_MARKER")):
                self.assertEqual(invoke(bytes(31),root/"closed-stderr"),1)
            self.assertFalse((root/"closed-stderr").exists())
            with patch("builtins.print", side_effect=ValueError("closed stream")):
                self.assertEqual(invoke(bytes(range(32)),root/"closed-stream"),0)
            self.assertTrue((root/"closed-stream"/"validation.json").is_file())
            # Exercise CPython's actual buffered-stream shutdown, rather than
            # replacing print: a late flush used to change success to exit 120.
            import subprocess
            import sys
            for error_stream in (False, True):
                destination = root / ("pipe-error" if error_stream else "pipe-success")
                script = (
                    "import sys; sys.path[:] = " + repr(sys.path) + "; "
                    "from projects.h2loader.tools.bazel.firmware_output import encrypted_bk_recovery_main; "
                    "sys.exit(encrypted_bk_recovery_main(" + repr(args(0,destination)) + "))"
                )
                reader, writer = os.pipe()
                os.close(reader)
                try:
                    with subprocess.Popen(
                        [sys.executable,"-c",script], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE if error_stream else writer,
                        stderr=writer if error_stream else subprocess.PIPE,
                    ) as child:
                        os.close(writer)
                        writer = None
                        stdout, stderr = child.communicate(bytes(31) if error_stream else bytes(range(32)),timeout=30)
                        self.assertEqual(child.returncode,1 if error_stream else 0,(stdout,stderr))
                        self.assertEqual(stdout if error_stream else stderr,b"")
                finally:
                    if writer is not None:
                        os.close(writer)
                self.assertEqual(destination.exists(),not error_stream)
            for secret in [b"",bytes(31),bytes(33)]:
                self.assertEqual(invoke(secret,root/"bad"),1)
                self.assertFalse((root/"bad").exists())
            self.assertEqual(invoke(bytes(range(32)),root/"partial",True),1)
            self.assertFalse((root/"partial").exists())
            self.assertFalse(list(root.glob(".bk-recovery-*")))
            with (root/"source.bin").open("rb") as regular, patch("projects.h2loader.tools.bazel.firmware_output._protect_recovery_process"), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(encrypted_bk_recovery_main(args(regular.fileno(),root/"file-key")),1)
            self.assertFalse((root/"file-key").exists())
            with patch("shutil.rmtree", side_effect=OSError("SECRET_MARKER")):
                self.assertEqual(invoke(bytes(range(32)),root/"cleanup-failure",True),1)
            self.assertFalse((root/"cleanup-failure").exists())
            self.assertTrue(list(root.glob(".bk-recovery-*")))



if __name__ == "__main__":
    unittest.main()
