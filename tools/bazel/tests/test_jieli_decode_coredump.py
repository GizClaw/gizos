"""Tests for post-mortem RAM log decoding (no live board required)."""

import importlib.util
from pathlib import Path
import struct
import unittest


spec = importlib.util.spec_from_file_location(
    "jieli_decode_coredump", Path(__file__).resolve().parents[1] / "jieli_decode_coredump.py"
)
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


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
