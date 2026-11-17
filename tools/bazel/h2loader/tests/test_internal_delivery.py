"""Offline identity/integrity and create-only private upload regressions."""

import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.bazel.h2loader.firmware_artifacts import package_manifest, write_package
from tools.bazel.h2loader.internal_delivery import (
    BUCKET, IDENTITY, STEM, digest, upload, validate_package,
)


class DeliveryTest(unittest.TestCase):
    def setUp(self):
        self.version = "0.2.0-ac791n-devkit.1"
        image = b"synthetic AC791N payload"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "package"
            write_package(path, "app/jieli/update.ufw", image, [],
                          **IDENTITY, version=self.version, package_format=1)
            self.package = path.read_bytes()
        self.metadata = {
            **IDENTITY, "version": self.version, "image": "fruit-slice",
            "package_format": 1,
            "package_manifest": package_manifest(image, **IDENTITY,
                                                 version=self.version, package_format=1),
            "assets": [{"operation": "managed-install", "name": STEM + ".update.tar.zlib",
                        "size": len(self.package), "sha256": digest(self.package)}],
        }

    def test_matching_package(self):
        result = validate_package(self.package, json.dumps(self.metadata).encode(), self.version)
        self.assertEqual(result["version"], self.version)

    def test_reject_mismatched_metadata(self):
        for field, value in (("board", "amoled"), ("version", "0.1.1"),
                             ("package_format", 2), ("image", "loader")):
            with self.subTest(field=field):
                metadata = {**self.metadata, field: value}
                with self.assertRaises(ValueError):
                    validate_package(self.package, json.dumps(metadata).encode(), self.version)
        for field, value in (("sha256", "0" * 64), ("size", 1), ("name", "other")):
            metadata = copy.deepcopy(self.metadata)
            metadata["assets"][0][field] = value
            with self.assertRaises(ValueError):
                validate_package(self.package, json.dumps(metadata).encode(), self.version)

    def test_reject_corrupt_payload_and_manifest(self):
        with self.assertRaises(ValueError):
            validate_package(self.package[:-5], json.dumps(self.metadata).encode(), self.version)
        self.metadata["package_manifest"]["image_sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            validate_package(self.package, json.dumps(self.metadata).encode(), self.version)

    def test_upload_is_private_create_only(self):
        receipt = {"bucket": BUCKET, "name": "internal-delivery/example", "size": "3"}
        with patch("urllib.request.urlopen") as request:
            request.return_value.__enter__.return_value.read.return_value = json.dumps(receipt).encode()
            upload(receipt["name"], b"abc", "test-token", "application/octet-stream")
            sent = request.call_args.args[0]
            self.assertIn("ifGenerationMatch=0", sent.full_url)
            self.assertIn("storage.googleapis.com/upload/storage/v1/b/" + BUCKET, sent.full_url)
            self.assertNotIn("test-token", sent.full_url)
            self.assertNotIn("predefinedAcl", sent.full_url)


if __name__ == "__main__":
    unittest.main()
