"""Run native firmware encryption through the public CLI and generated rules."""

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from python.runfiles import Runfiles
from tools.bazel import bk7258_encrypt
from tools.bazel.bk7258_flash_aes import BekenXTS, flash_crc
from tools.bazel.tests.bk7258_encryption.fixture import fixture


KEY = bytes(range(32))


class NativeEncryptionTest(unittest.TestCase):
    def executable(self, label):
        path = Runfiles.Create().Rlocation("gizos/" + label)
        self.assertIsNotNone(path)
        self.assertTrue(Path(path).is_file(), path)
        return path

    def assert_image(self, output, kind, source):
        filename = "app.flash-aes.rbl" if kind == "app" else "full.flash-aes.bin"
        self.assertEqual({p.name for p in output.iterdir()}, {filename, "validation.json"})
        encrypted = (output / filename).read_bytes()
        self.assertNotEqual(source, encrypted)
        self.assertEqual(len(source), len(encrypted))
        receipt = json.loads((output / "validation.json").read_text())
        self.assertEqual(receipt["image"], kind)
        self.assertEqual(receipt["source_image_sha256"], hashlib.sha256(source).hexdigest())
        self.assertEqual(receipt["image_sha256"], hashlib.sha256(encrypted).hexdigest())
        base = 0x240000 if kind == "app" else 0
        self.assertEqual(receipt["logical_base"], base)
        self.assertEqual(receipt["physical_base"], base // 32 * 34)
        cipher = BekenXTS(KEY)
        plain = b"".join(source[i:i + 32] for i in range(0, len(source), 34))
        restored = b"".join(cipher.block(encrypted[i:i + 32], base + i // 34 * 32, True)
                             for i in range(0, len(encrypted), 34))
        if kind == "full":
            self.assertEqual(encrypted[272:288], source[272:288])
            self.assertEqual(restored[:256], plain[:256])
            self.assertEqual(restored[272:], plain[272:])
        else:
            self.assertEqual(restored, plain)
        for i in range(0, len(encrypted), 34):
            self.assertEqual(encrypted[i + 32:i + 34], flash_crc(encrypted[i:i + 32]))
        self.assertEqual(output.stat().st_mode & 0o777, 0o700)
        for path in output.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_non_h2loader_native_rule_both_image_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            for kind in ("app", "full"):
                with self.subTest(kind=kind):
                    output = Path(directory) / kind
                    executable = self.executable("tools/bazel/tests/bk7258_encryption/" + kind + "_encrypted")
                    result = subprocess.run([executable, "--key-fd", "0", "--output-dir", str(output)],
                                            input=KEY, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.assert_image(output, kind, fixture(kind)[0])
                    # Native mode is fixed in the rule rather than overridable by argv.
                    override = subprocess.run([executable, "--image", "full", "--key-fd", "0",
                                               "--output-dir", str(Path(directory) / (kind + "-override"))],
                                              input=KEY, capture_output=True, timeout=30)
                    self.assertNotEqual(override.returncode, 0)

    def inputs(self, root, kind="app"):
        source, metadata = fixture(kind)
        path = root / "source.bin"
        path.write_bytes(source)
        (root / "partitions.json").write_text(json.dumps(metadata))
        return source, path

    def arguments(self, root, output="output"):
        return ["--source", str(root / "source.bin"), "--metadata", str(root), "--image", "app",
                "--key-fd", "0", "--output-dir", str(root / output)]

    def test_public_cli_key_failures_never_publish_plaintext(self):
        tool = self.executable("tools/bazel/bk7258_encrypt")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, path = self.inputs(root)
            result = subprocess.run([tool, *self.arguments(root)], input=KEY,
                                    capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assert_image(root / "output", "app", source)
            for length in (0, 16, 31, 33):
                with self.subTest(length=length):
                    destination = "bad-" + str(length)
                    result = subprocess.run([tool, *self.arguments(root, destination)], input=bytes(length),
                                            capture_output=True, timeout=30)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((root / destination).exists())
                    self.assertNotIn(b"Traceback", result.stderr)
            self.assertEqual(path.read_bytes(), source)
            self.assertFalse(list(root.glob(".bk-firmware-*")))

    def test_closed_diagnostic_pipes_keep_publication_status(self):
        tool = self.executable("tools/bazel/bk7258_encrypt")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.inputs(root)
            for error_stream in (False, True):
                destination = "closed-error" if error_stream else "closed-output"
                reader, writer = os.pipe()
                os.close(reader)
                try:
                    with subprocess.Popen(
                        [tool, *self.arguments(root, destination)], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE if error_stream else writer,
                        stderr=writer if error_stream else subprocess.PIPE,
                    ) as child:
                        os.close(writer)
                        writer = None
                        stdout, stderr = child.communicate(bytes(31) if error_stream else KEY, timeout=30)
                        self.assertEqual(child.returncode, 1 if error_stream else 0, (stdout, stderr))
                        self.assertEqual(stdout if error_stream else stderr, b"")
                finally:
                    if writer is not None:
                        os.close(writer)
                self.assertEqual((root / destination).exists(), not error_stream)

    def test_validation_precedes_key_and_rejects_bazel_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, path = self.inputs(root)
            with patch.object(bk7258_encrypt, "protect_process"), patch.object(bk7258_encrypt, "read_key") as key:
                with redirect_stderr(io.StringIO()):
                    path.write_bytes(source[:-1])
                    self.assertEqual(bk7258_encrypt.main(self.arguments(root)), 1)
                    key.assert_not_called()
                    path.write_bytes(source)
                    (root / "bazel-out").mkdir()
                    args = self.arguments(root)
                    args[-1] = str(root / "bazel-out/output")
                    self.assertEqual(bk7258_encrypt.main(args), 1)
                    key.assert_not_called()
            self.assertFalse((root / "output").exists())

    def test_failed_process_protection_never_reads_the_key(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.inputs(root)
            with patch.object(bk7258_encrypt, "protect_process", side_effect=ValueError("DUMP_PROTECTION_FAILED")), patch.object(bk7258_encrypt, "read_key") as key, redirect_stderr(io.StringIO()):
                self.assertEqual(bk7258_encrypt.main(self.arguments(root)), 1)
                key.assert_not_called()
            self.assertFalse((root / "output").exists())

    def test_sources_are_not_reopened_after_reading_key(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, path = self.inputs(root)
            def read_key(_fd):
                path.write_bytes(b"changed after validation")
                (root / "partitions.json").write_text("{}")
                return bytearray(KEY)
            with patch.object(bk7258_encrypt, "protect_process"), patch.object(bk7258_encrypt, "read_key", read_key), redirect_stdout(io.StringIO()):
                self.assertEqual(bk7258_encrypt.main(self.arguments(root)), 0)
            self.assert_image(root / "output", "app", source)

    def test_publication_failure_removes_private_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.inputs(root)
            with patch.object(bk7258_encrypt, "protect_process"), patch.object(bk7258_encrypt, "read_key", return_value=bytearray(KEY)), patch("os.fchmod", side_effect=OSError("fixture")), redirect_stderr(io.StringIO()):
                self.assertEqual(bk7258_encrypt.main(self.arguments(root)), 1)
            self.assertFalse((root / "output").exists())
            self.assertFalse(list(root.glob(".bk-firmware-*")))


if __name__ == "__main__":
    unittest.main()
