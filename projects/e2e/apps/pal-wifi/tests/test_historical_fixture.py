"""Bound the old AMOLED test tool to its actual archived source and package."""
import copy
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path("projects/e2e/apps/pal-wifi").resolve()))
from receipt_bindings import (AMOLED_R15_VERSION, AMOLED_R15_PACKAGE, AMOLED_R15_IMAGE,
                              AMOLED_R15_HISTORICAL_SOURCES, _historical_source_policy,
                              _verify_historical_git_blobs, _check_sources)


class HistoricalFixture(unittest.TestCase):
    def setUp(self):
        self.report = dict(platform="amoled-fixture", version=AMOLED_R15_VERSION,
                           package_sha256=AMOLED_R15_PACKAGE, image_sha256=AMOLED_R15_IMAGE,
                           source_inputs={path: info["sha256"] for path, info in
                                          AMOLED_R15_HISTORICAL_SOURCES.items()},
                           historical_source_inputs=copy.deepcopy(AMOLED_R15_HISTORICAL_SOURCES))

    def test_exact_pinned_r15_identity(self):
        self.assertEqual(_historical_source_policy(self.report), AMOLED_R15_HISTORICAL_SOURCES)

    def test_wrong_version_package_image_commit_and_hash_rejected(self):
        variants = []
        for field, value in (("version", "fixture-r16"), ("package_sha256", "0" * 64),
                             ("image_sha256", "0" * 64)):
            changed = copy.deepcopy(self.report)
            changed[field] = value
            variants.append(changed)
        path = next(iter(AMOLED_R15_HISTORICAL_SOURCES))
        for key in ("commit", "sha256"):
            changed = copy.deepcopy(self.report)
            changed["historical_source_inputs"][path][key] = "0" * 40 if key == "commit" else "0" * 64
            variants.append(changed)
        changed = copy.deepcopy(self.report)
        changed["source_inputs"][path] = "0" * 64
        variants.append(changed)
        for report in variants:
            with self.assertRaises(AssertionError): _historical_source_policy(report)

    def test_dut_cannot_claim_historical_fixture_exemption(self):
        for platform in ("devkit", "bk7258", "macos"):
            changed = copy.deepcopy(self.report)
            changed["platform"] = platform
            with self.assertRaises(AssertionError): _historical_source_policy(changed)

    def test_live_git_blob_mismatch_rejected(self):
        with mock.patch("receipt_bindings.subprocess.check_output", return_value=b"wrong historical bytes"):
            with self.assertRaises(AssertionError):
                _verify_historical_git_blobs(AMOLED_R15_HISTORICAL_SOURCES)

    def test_other_current_source_drift_still_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "new-source.c"
            path.write_bytes(b"before")
            sources = dict(self.report["source_inputs"])
            sources[str(path)] = hashlib.sha256(b"before").hexdigest()
            _check_sources(sources, AMOLED_R15_HISTORICAL_SOURCES)
            path.write_bytes(b"after")
            with self.assertRaises(AssertionError):
                _check_sources(sources, AMOLED_R15_HISTORICAL_SOURCES)


if __name__ == "__main__": unittest.main()
