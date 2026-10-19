"""Validate actual old/new Bazel package actions against shared native bytes."""
import hashlib
import io
import json
from pathlib import Path
import sys
import shlex
import tarfile
import unittest
import zlib

FILES = [Path(path) for argument in sys.argv[1:] for path in shlex.split(argument)]

class PackageRulesTest(unittest.TestCase):
    def test_parallel_rules_preserve_native_identity(self):
        metadata = [json.loads(path.read_text()) for path in FILES if path.name.endswith(".firmware.json")]
        self.assertEqual(len(metadata), 2)
        metadata.sort(key=lambda entry: entry["package_format"])
        raw_images = []
        for expected_format, info in enumerate(metadata, 1):
            self.assertEqual(info["package_format"], expected_format)
            self.assertEqual(info["package_manifest"]["format"], expected_format)
            asset = info["assets"][0]
            package = next(path for path in FILES if path.name == asset["name"])
            self.assertEqual(hashlib.sha256(package.read_bytes()).hexdigest(), asset["sha256"])
            if expected_format == 1:
                with tarfile.open(fileobj=io.BytesIO(zlib.decompress(package.read_bytes())), mode="r:") as tar:
                    raw = tar.extractfile("app/esp/app.bin").read()
            else:
                with tarfile.open(package, mode="r:") as tar:
                    self.assertEqual(tar.getnames(), ["manifest", "data.tar.zlib", "app.bin.zlib"])
                    raw = zlib.decompress(tar.extractfile("app.bin.zlib").read())
            self.assertEqual(hashlib.sha256(raw).hexdigest(), info["package_manifest"]["image_sha256"])
            raw_images.append(raw)
        self.assertEqual(raw_images[0], raw_images[1])
        self.assertEqual(metadata[0]["native_artifacts"], metadata[1]["native_artifacts"])

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
