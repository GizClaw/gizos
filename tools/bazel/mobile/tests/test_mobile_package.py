"""Verify Java/JNI SDK envelopes through the production package entry."""
import argparse
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from tools.bazel.mobile import mobile_package


def android_elf():
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", header, 18, 183)  # EM_AARCH64
    struct.pack_into("<Q", header, 40, 64)  # Section-header offset.
    struct.pack_into("<HH", header, 58, 64, 1)
    description = struct.pack("<I", 31) + b"r28c" + bytes(60)
    note = struct.pack("<III", 8, len(description), 1) + b"Android\0" + description
    section = bytearray(64)
    struct.pack_into("<I", section, 4, 7)  # SHT_NOTE
    struct.pack_into("<QQ", section, 24, 128, len(note))
    return bytes(header + section + note)


class AndroidClassesPackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.binary = self.root / "libh2_pal_core.so"
        self.binary.write_bytes(android_elf())

    def args(self, name, classes=None, rules=None):
        return argparse.Namespace(kind="android", binary=str(self.binary),
                                  name="h2-pal-core", version="2.0.0", header=[],
                                  artifact=str(self.root / (name + ".aar")),
                                  bundle=str(self.root / name),
                                  classes_jar=str(classes) if classes else None,
                                  consumer_proguard=str(rules) if rules else None)

    def test_classes_and_native_callback_rules_are_shipped(self):
        classes = self.root / "view.jar"
        payload = mobile_package.archive_bytes({"com/example/Platform.class": b"class-payload"})
        classes.write_bytes(payload)
        rules = self.root / "consumer.pro"
        rules.write_text("-keepclassmembers class com.example.Platform { void requestFrame(); }\n")
        args = self.args("with-java", classes, rules)
        mobile_package.package(args)
        with zipfile.ZipFile(args.artifact) as aar:
            self.assertEqual(aar.read("classes.jar"), payload)
            self.assertEqual(aar.read("proguard.txt"), rules.read_bytes())
            self.assertEqual(aar.read("jni/arm64-v8a/libh2_pal_core.so"), self.binary.read_bytes())
            with zipfile.ZipFile(io.BytesIO(aar.read("classes.jar"))) as jar:
                self.assertEqual(jar.read("com/example/Platform.class"), b"class-payload")

    def test_existing_native_only_callers_keep_an_empty_jar(self):
        args = self.args("native-only")
        mobile_package.package(args)
        with zipfile.ZipFile(args.artifact) as aar:
            self.assertNotIn("proguard.txt", aar.namelist())
            with zipfile.ZipFile(io.BytesIO(aar.read("classes.jar"))) as jar:
                self.assertEqual(jar.namelist(), [])

    def test_invalid_classes_jar_does_not_emit_an_aar(self):
        classes = self.root / "invalid.jar"
        classes.write_bytes(b"not a jar")
        args = self.args("invalid", classes)
        with self.assertRaises(zipfile.BadZipFile):
            mobile_package.package(args)
        self.assertFalse(Path(args.artifact).exists())

    def test_repeated_packaging_is_byte_identical(self):
        classes = self.root / "view.jar"
        classes.write_bytes(mobile_package.archive_bytes({"Platform.class": b"class-payload"}))
        first, second = self.args("first", classes), self.args("second", classes)
        mobile_package.package(first)
        mobile_package.package(second)
        self.assertEqual(Path(first.artifact).read_bytes(), Path(second.artifact).read_bytes())


if __name__ == "__main__":
    unittest.main()
