"""Exercise the manual Release gate and asset validation without dispatching."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools.bazel import release_workflow


ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github/workflows/release.yml"
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

    def test_every_gizos_checkout_and_validation_step_is_wired(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        checkout_blocks = text.split("- name: Check out GizOS")[1:]
        self.assertGreaterEqual(len(checkout_blocks), 7)
        for block in checkout_blocks:
            self.assertIn("ref: ${{ github.sha }}", block.split("\n      - ", 1)[0])
        self.assertIn("release_workflow.py gate", text)
        self.assertIn("release_workflow.py prepare", text)
        self.assertIn("release_workflow.py verify", text)
        self.assertLess(text.index("name: Require default branch release ref"),
                        text.index("name: Check out GizOS"))
        self.assertIn('"$GITHUB_REF" != "refs/heads/$DEFAULT_BRANCH"', text)
        self.assertLess(text.index("release_workflow.py gate"), text.index("name: Build catalog slice"))
        self.assertLess(text.index("release_workflow.py verify"), text.index('gh release edit "$RELEASE_TAG"'))

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


if __name__ == "__main__":
    unittest.main()
