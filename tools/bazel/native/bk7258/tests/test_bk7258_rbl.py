"""Exercise the native paired-image RBL wire format and build-tool entry."""

import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


TOOL = Path(__file__).resolve().parents[1] / "bk7258_rbl.py"
SPEC = importlib.util.spec_from_file_location("bk7258_rbl", TOOL)
RBL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RBL)


class RblTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "app.bin"
        self.source.write_bytes(b"hello")
        os.utime(self.source, (1700000000, 1700000000))
        self.output = self.root / "app.rbl"
        self.metadata = self.root / "configurationab.json"
        self.metadata.write_text(json.dumps({"section": [
            {"partition": "bootloader", "size": "64K"},
            {"partition": "app", "size": "8K"},
        ]}))

    def test_native_header_payload_and_footer(self):
        RBL.pack(self.source, self.output, self.metadata)
        data = self.output.read_bytes()
        self.assertEqual(len(data), 8192)
        self.assertEqual(data[:5], b"hello")
        self.assertEqual(data[5:4096], b"\xff" * 4091)
        head = data[4096:4192]
        fields = struct.unpack("<4sH2xI16s24s24sIIIII", head)
        self.assertEqual(fields[:3], (b"RBL\0", 0, 1700000000))
        self.assertEqual(fields[3].rstrip(b"\0"), b"app")
        self.assertEqual(fields[4].rstrip(b"\0"), b"2")
        self.assertEqual(fields[6:10], (0x3610A686, 0x4F9F2CAB, 5, 5))
        self.assertEqual(fields[10], zlib.crc32(head[:92]))
        self.assertEqual(data[4192:], b"\xff" * (8192 - 4192))

    def test_oversize_and_empty_images_leave_no_output(self):
        for payload in (b"", b"x" * 4096, b"x" * 4097):
            with self.subTest(size=len(payload)):
                self.source.write_bytes(payload)
                with self.assertRaises(ValueError):
                    RBL.pack(self.source, self.output, self.metadata)
                self.assertFalse(self.output.exists())

    def test_invalid_partition_and_identity(self):
        for size in ("4K", "0", "17M", "-1", "invalid"):
            with self.subTest(size=size), self.assertRaises(ValueError):
                RBL.partition_size(size)
        for firmware, version in (("x" * 17, "2"), ("app", "x" * 25)):
            with self.assertRaises(ValueError):
                RBL.pack(self.source, self.output, self.metadata,
                         firmware=firmware, version=version)
        self.metadata.write_text('{"section": []}')
        with self.assertRaises(ValueError):
            RBL.pack(self.source, self.output, self.metadata)
        self.assertFalse(self.output.exists())

    def test_sdk_entry_uses_only_ordinary_format_configuration(self):
        config = self.root / "partitions/bk7258/ota_rbl.config"
        config.parent.mkdir(parents=True)
        environment = dict(os.environ, PARTITIONS_DIR=str(self.root))
        command = [sys.executable, "-I", str(TOOL), "-i", str(self.source),
                   "-o", str(self.output), "-pjd", str(self.root), "packager"]
        config.write_text('{"gzip": "0", "outf": "app.rbl"}')
        result = subprocess.run(command, env=environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.output.unlink()
        config.write_text('{"mode": "custom"}')
        result = subprocess.run(command, env=environment, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        config.unlink()
        result = subprocess.run(command + ["--mode", "custom"],
                                env=environment, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
