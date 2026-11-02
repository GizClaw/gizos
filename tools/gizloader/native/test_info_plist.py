"""Verify bundle identity and privacy keys in the linked executable sections."""

from pathlib import Path
import plistlib
import struct
import sys
import unittest


ARTIFACTS = (
    (Path(sys.argv[1]), Path(sys.argv[2]), "com.gizclaw.gizloader.cli", "gizloader-cli"),
    (Path(sys.argv[3]), Path(sys.argv[4]), "com.gizclaw.gizloader.session", "gizloader-session"),
)


def embedded_plist(path):
    data = path.read_bytes()
    if data[:4] != b"\xcf\xfa\xed\xfe":
        raise AssertionError(f"{path}: expected a 64-bit little-endian Mach-O")
    command_count = struct.unpack_from("<I", data, 16)[0]
    offset = 32
    sections = []
    for _ in range(command_count):
        command, command_size = struct.unpack_from("<II", data, offset)
        if command == 0x19:  # LC_SEGMENT_64
            section_count = struct.unpack_from("<I", data, offset + 64)[0]
            for index in range(section_count):
                section = offset + 72 + 80 * index
                name, segment, _, size, position = struct.unpack_from(
                    "<16s16sQQI", data, section
                )
                if (name.rstrip(b"\0"), segment.rstrip(b"\0")) == (
                    b"__info_plist", b"__TEXT"
                ):
                    sections.append(plistlib.loads(data[position:position + size]))
        offset += command_size
    if len(sections) != 1:
        raise AssertionError(f"{path}: expected one embedded Info.plist, got {len(sections)}")
    return sections[0]


class NativeInfoPlistTest(unittest.TestCase):
    def test_linked_bundle_identity_and_bluetooth_descriptions(self):
        for binary, generated, identifier, executable in ARTIFACTS:
            with self.subTest(binary=binary):
                actual = embedded_plist(binary)
                self.assertEqual(actual, plistlib.loads(generated.read_bytes()))
                for key, value in {
                    "CFBundleIdentifier": identifier,
                    "CFBundleName": "Gizloader",
                    "CFBundleExecutable": executable,
                    "CFBundleVersion": "1",
                    "CFBundlePackageType": "APPL",
                }.items():
                    self.assertEqual(actual[key], value)
                description = actual["NSBluetoothAlwaysUsageDescription"]
                self.assertTrue(description.strip())
                self.assertEqual(actual["NSBluetoothPeripheralUsageDescription"], description)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
