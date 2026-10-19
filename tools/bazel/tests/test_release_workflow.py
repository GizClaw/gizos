"""Exercise the manual Release gate and asset validation without dispatching."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools.bazel import release_workflow


COMMIT = "a" * 40


class ReleaseWorkflowTest(unittest.TestCase):
    def test_default_branch_and_exact_checkout_are_required(self):
        release_workflow.require_snapshot("refs/heads/main", "main", COMMIT, COMMIT)
        for ref, checkout in (("refs/heads/feature", COMMIT),
                              ("refs/heads/main", "b" * 40),
                              ("refs/tags/v1", COMMIT)):
            with self.subTest(ref=ref, checkout=checkout), self.assertRaises(
                release_workflow.ReleaseWorkflowError
            ):
                release_workflow.require_snapshot(ref, "main", COMMIT, checkout)

    def make_assets(self, directory: Path) -> None:
        (directory / "firmware.zip").write_bytes(b"firmware")
        (directory / "package.tgz").write_bytes(b"npm")
        names = ("firmware.zip", "package.tgz")
        (directory / "SHA256SUMS").write_text(
            "".join(f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n"
                    for name in names), encoding="ascii")

    def test_prepared_index_and_downloaded_assets_are_verified(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.make_assets(directory)
            release_workflow.prepare(directory, "GizClaw/gizos", "v20260920-120000", COMMIT)
            index = json.loads((directory / "index.json").read_text())
            self.assertEqual(index["commit"], COMMIT)
            self.assertEqual([item["name"] for item in index["assets"]],
                             ["SHA256SUMS", "firmware.zip", "package.tgz"])
            release_workflow.verify(directory, "GizClaw/gizos", "v20260920-120000", COMMIT)
            with self.assertRaisesRegex(release_workflow.ReleaseWorkflowError, "index"):
                release_workflow.verify(directory, "GizClaw/gizos", "vwrong", COMMIT)
            (directory / "package.tgz").write_bytes(b"bad")
            with self.assertRaisesRegex(release_workflow.ReleaseWorkflowError, "checksum"):
                release_workflow.verify(directory, "GizClaw/gizos", "v20260920-120000", COMMIT)

    def test_missing_extra_and_malformed_assets_fail_closed(self):
        for mutation in ("missing", "extra", "malformed", "symlink"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp)
                self.make_assets(directory)
                if mutation == "missing":
                    (directory / "package.tgz").unlink()
                elif mutation == "extra":
                    (directory / "unexpected").write_bytes(b"extra")
                elif mutation == "malformed":
                    (directory / "SHA256SUMS").write_text("bad\n")
                else:
                    (directory / "alias").symlink_to("package.tgz")
                with self.assertRaises(release_workflow.ReleaseWorkflowError):
                    release_workflow.prepare(directory, "GizClaw/gizos", "v20260920-120000", COMMIT)
                self.assertFalse((directory / "index.json").exists())

    def test_gizos_release_metadata_must_match_trigger_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.make_assets(directory)
            metadata = directory / "gizos-release.json"
            metadata.write_text(json.dumps({
                "release_id": "20260920-120000",
                "release_tag": "v20260920-120000",
                "commit": COMMIT,
            }))
            names = ("firmware.zip", "gizos-release.json", "package.tgz")
            (directory / "SHA256SUMS").write_text(
                "".join(f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n"
                        for name in names), encoding="ascii")
            release_workflow.prepare(directory, "GizClaw/gizos", "v20260920-120000", COMMIT)
            release_workflow.verify(directory, "GizClaw/gizos", "v20260920-120000", COMMIT)
            with self.assertRaisesRegex(release_workflow.ReleaseWorkflowError, "trigger identity"):
                release_workflow.verify(directory, "GizClaw/gizos", "v20260920-120000", "b" * 40)


if __name__ == "__main__":
    unittest.main()
