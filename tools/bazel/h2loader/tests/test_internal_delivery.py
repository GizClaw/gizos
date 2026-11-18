"""Offline identity/integrity and create-only private upload regressions."""

import copy
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.bazel.h2loader.firmware_artifacts import package_manifest, write_package
from tools.bazel.h2loader.internal_delivery import (
    BUCKET, IDENTITY, STEM, digest, main, upload, validate_compilation_mode, validate_package,
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

    def test_changed_source_stops_before_upload(self):
        source = "a" * 40
        with patch.dict(os.environ, {"SOURCE_SHA": source, "WORKFLOW_SHA": "b" * 40}), \
                patch("sys.argv", ["delivery", "--outputs", "/unused"]), \
                patch("subprocess.check_output", side_effect=[source + "\n", "MODULE.bazel.lock\n"]), \
                patch("urllib.request.urlopen") as request:
            with self.assertRaisesRegex(ValueError, "tracked source changed.*MODULE.bazel.lock"):
                main()
            request.assert_not_called()

    def test_compilation_mode_receipt_validation(self):
        self.assertEqual(validate_compilation_mode("fastbuild"), "fastbuild")
        for mode in ("fastbuild", "opt"):
            self.assertEqual(validate_compilation_mode(mode, {"requested": mode, "observed": mode}), mode)
        for mode, receipt in (
            ("dbg", None), ("", None), ("opt", None),
            ("opt", {"requested": "opt", "observed": "fastbuild"}),
            ("fastbuild", {"requested": "opt", "observed": "opt"}),
        ):
            with self.subTest(mode=mode, receipt=receipt), self.assertRaises(ValueError):
                validate_compilation_mode(mode, receipt)

    def test_delivery_records_verified_mode_and_keeps_default(self):
        for mode, with_receipt in ((None, False), ("fastbuild", True), ("opt", True)):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                package = root / (STEM + ".update.tar.zlib")
                metadata = root / (STEM + ".firmware.json")
                package.write_bytes(self.package)
                metadata.write_text(json.dumps(self.metadata))
                outputs = root / "outputs.txt"
                outputs.write_text(str(package) + "\n" + str(metadata) + "\n")
                argv = ["delivery", "--outputs", str(outputs)]
                receipt = None
                if with_receipt:
                    receipt = {"requested": mode, "observed": mode}
                    receipt_file = root / "mode.json"
                    receipt_file.write_text(json.dumps(receipt))
                    argv.extend(["--mode-receipt", str(receipt_file)])
                source = "a" * 40
                environment = {
                    "SOURCE_SHA": source, "WORKFLOW_SHA": "b" * 40,
                    "EXPECTED_VERSION": self.version, "GITHUB_RUN_ID": "123",
                    "GITHUB_RUN_ATTEMPT": "1", "GITHUB_REPOSITORY": "GizClaw/gizos",
                    "DELIVERY_TOKEN": "offline-test-token",
                }
                if mode is not None:
                    environment["DELIVERY_COMPILATION_MODE"] = mode
                with patch.dict(os.environ, environment, clear=True), patch("sys.argv", argv), \
                        patch("subprocess.check_output", side_effect=[source + "\n", ""]), \
                        patch("subprocess.run"), \
                        patch("tools.bazel.h2loader.internal_delivery.upload") as sent, \
                        patch("sys.stdout", new_callable=io.StringIO):
                    main()
                payloads = {Path(call.args[0]).name: call.args[1] for call in sent.call_args_list}
                self.assertEqual(set(payloads), {package.name, metadata.name, "build-record.json"})
                record = json.loads(payloads["build-record.json"])
                self.assertEqual(record["compilation_mode"], mode or "fastbuild")
                self.assertEqual(record["compilation_mode_receipt"], receipt)
                self.assertEqual(record["source_sha"], source)
                self.assertEqual(payloads[package.name], self.package)

    def test_invalid_mode_stops_before_upload(self):
        with patch.dict(os.environ, {"DELIVERY_COMPILATION_MODE": "dbg"}), \
                patch("sys.argv", ["delivery", "--outputs", "/unused"]), \
                patch("urllib.request.urlopen") as request:
            with self.assertRaisesRegex(ValueError, "unsupported delivery compilation mode"):
                main()
            request.assert_not_called()

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
