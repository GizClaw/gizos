import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import hashlib

from tools.bazel.npm_release import pack

SOURCE = Path(__file__).resolve().parents[1] / "publish.py"
SPEC = importlib.util.spec_from_file_location("publish_gizos", SOURCE)
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)
BATCH = "20261030-012345"


class PublishTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        package = self.root / "package"
        (package / "bin").mkdir(parents=True)
        manifest = package / "package.json"
        manifest.write_text(json.dumps({
            "name": publisher.PACKAGE,
            "version": publisher.release_version(BATCH),
            "files": ["bin/gizos.js"],
            "bin": {"gizos": "bin/gizos.js"},
            "publishConfig": {"registry": publisher.REGISTRY},
        }))
        (package / "bin/gizos.js").write_text("#!/usr/bin/env bun\n")
        self.output = self.root / "release"
        self.tarball = pack(package, manifest, self.output)
        content = self.tarball.read_bytes()
        (self.output / "npm-index.json").write_text(json.dumps({
            "format": 1, "version": BATCH, "package_count": 1,
            "packages": [{
                "name": publisher.PACKAGE, "version": publisher.release_version(BATCH),
                "tarball": self.tarball.name, "size": len(content),
                "sha256": hashlib.sha256(content).hexdigest(),
            }],
        }))

    def test_publication_uses_exact_release_tarball_and_verifies_integrity(self):
        integrity = publisher.checked_tarball(self.output, BATCH)[2]
        calls = []

        def run(args):
            calls.append(args)
            if len(calls) == 1:
                return subprocess.CompletedProcess(args, 1, '{"error":{"code":"E404"}}', "")
            if args[1] == "publish":
                return subprocess.CompletedProcess(args, 0, "", "")
            return subprocess.CompletedProcess(args, 0, json.dumps(integrity), "")

        self.assertIn("Published and verified", publisher.publish(self.output, BATCH, run))
        self.assertEqual(calls[1][2], str(self.tarball))
        self.assertIn("--provenance", calls[1])
        self.assertIn("--ignore-scripts", calls[1])

    def test_matching_existing_version_is_idempotent(self):
        integrity = publisher.checked_tarball(self.output, BATCH)[2]
        calls = []

        def run(args):
            calls.append(args)
            return subprocess.CompletedProcess(args, 0, json.dumps(integrity), "")

        self.assertIn("already published", publisher.publish(self.output, BATCH, run))
        self.assertEqual(len(calls), 1)

    def test_existing_different_version_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "different content"):
            publisher.publish(self.output, BATCH, lambda args: subprocess.CompletedProcess(args, 0, '"sha512-other"', ""))

    def test_registry_failure_is_not_treated_as_missing_package(self):
        with self.assertRaisesRegex(ValueError, "cannot query npm"):
            publisher.publish(self.output, BATCH, lambda args: subprocess.CompletedProcess(args, 1, '{"error":{"code":"E401"}}', "auth failed"))

    def test_corrupt_or_wrong_release_is_not_published(self):
        with self.assertRaisesRegex(ValueError, "does not belong"):
            publisher.checked_tarball(self.output, "20261031-012345")
        self.tarball.write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "checksum"):
            publisher.checked_tarball(self.output, BATCH)


if __name__ == "__main__":
    unittest.main()
