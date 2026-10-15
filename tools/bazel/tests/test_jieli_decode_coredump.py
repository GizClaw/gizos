"""Tests for post-mortem RAM log decoding (no live board required)."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    "jieli_decode_coredump", Path(__file__).parents[1] / "jieli_decode_coredump.py"
)
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


class CoredumpMainTest(unittest.TestCase):
    LOG = b"boot\ncommit complete\n"

    def record(self, *, version=decoder.VERSION, committed=decoder.COMMITTED,
               corrupt_checksum=False):
        header = struct.pack(
            decoder.HEADER_FORMAT, decoder.RECORD_SIZE, decoder.MAGIC, version,
            7, 2, 3, 0x1234, 0, len(self.LOG), len(self.LOG),
        )
        # Nonzero padding makes accidentally printing beyond log_bytes visible.
        payload = header + self.LOG.ljust(decoder.LOG_CAPACITY, b"x")
        checksum = decoder.checksum(payload) ^ int(corrupt_checksum)
        return payload + struct.pack("<2I", checksum, committed)

    def invoke(self, image, *arguments):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "flash.bin"
            path.write_bytes(image)
            stdout, stderr = io.StringIO(), io.StringIO()
            with mock.patch.object(decoder.sys, "argv", ["decoder", str(path), *arguments]), \
                    redirect_stdout(stdout), redirect_stderr(stderr):
                result = decoder.main()
            return result, stdout.getvalue(), stderr.getvalue()

    def test_valid_committed_record_and_exact_log(self):
        result, stdout, stderr = self.invoke(self.record(), "--show-log")
        self.assertEqual(result, 0)
        self.assertEqual(stderr, "")
        status, log_output = stdout.split("\n", 1)
        self.assertIn("valid=true", status.split())
        self.assertEqual(log_output.encode(), b"H2_JIELI_COREDUMP_LOG_BEGIN\n"
                         + self.LOG + b"H2_JIELI_COREDUMP_LOG_END\n")

    def test_record_at_flash_offset(self):
        image = b"\xff" * 256 + self.record() + b"trailing flash data"
        result, stdout, stderr = self.invoke(image, "--offset", "0x100")
        self.assertEqual(result, 0)
        self.assertIn("valid=true", stdout.split())
        self.assertNotIn("H2_JIELI_COREDUMP_LOG_BEGIN", stdout)
        self.assertEqual(stderr, "")

    def test_invalid_record_fields(self):
        # Recompute checksum for the version case to isolate version validation.
        for overrides in ({"corrupt_checksum": True}, {"committed": 0}, {"version": 3}):
            with self.subTest(overrides=overrides):
                result, stdout, stderr = self.invoke(self.record(**overrides))
                self.assertEqual(result, 1)
                self.assertIn("valid=false", stdout.split())
                self.assertEqual(stderr, "")

    def test_truncated_record(self):
        image = self.record()[:-1]
        result, stdout, stderr = self.invoke(image)
        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertEqual(stderr, f"invalid: need {decoder.RECORD_SIZE} bytes, got {len(image)}\n")

    def test_invalid_retained_ring(self):
        result, stdout, stderr = self.invoke(bytes(decoder.RETAINED_SIZE), "--retained-ring")
        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("invalid retained ring magic=0x00000000", stderr)


class RetainedLogTest(unittest.TestCase):
    def snapshot(self, written):
        capacity = decoder.LOG_CAPACITY
        ring = bytearray(capacity)
        for index, byte in enumerate(written):
            ring[index % capacity] = byte
        return struct.pack(
            "<3I", decoder.RETAINED_LOG_MAGIC, len(written) % capacity, len(written)
        ) + ring

    def test_short_log(self):
        text = b"boot\ncommit-enter\n"
        self.assertEqual(decoder.decode_retained_log(self.snapshot(text)),
                         (text, len(text), len(text)))

    def test_wrapped_log_keeps_latest_bytes_in_order(self):
        text = bytes(range(256)) * 10 + b"last-message\n"
        log, head, total = decoder.decode_retained_log(self.snapshot(text))
        self.assertEqual(log, text[-decoder.LOG_CAPACITY:])
        self.assertEqual(head, len(text) % decoder.LOG_CAPACITY)
        self.assertEqual(total, len(text))

    def test_loader_role_and_interrupted_write(self):
        valid = bytearray(self.snapshot(b"loader"))
        struct.pack_into("<I", valid, 0, decoder.RETAINED_LOG_MAGIC | 1)
        self.assertEqual(decoder.decode_retained_log(valid)[0], b"loader")
        struct.pack_into("<I", valid, 0, decoder.RETAINED_LOG_MAGIC | 0x80000001)
        with self.assertRaises(ValueError):
            decoder.decode_retained_log(valid)

    def test_empty_log(self):
        self.assertEqual(decoder.decode_retained_log(self.snapshot(b"")), (b"", 0, 0))

    def test_rejects_truncated_uninitialized_and_invalid_head(self):
        valid = self.snapshot(b"x")
        invalid_head = struct.pack("<3I", decoder.RETAINED_LOG_MAGIC,
                                   decoder.LOG_CAPACITY, 1) + valid[12:]
        for data in (valid[:-1], bytes(len(valid)), invalid_head):
            with self.subTest(data=data[:12]), self.assertRaises(ValueError):
                decoder.decode_retained_log(data)


if __name__ == "__main__":
    unittest.main()
