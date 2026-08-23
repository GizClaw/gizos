from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.bazel import toolchain_identity


class ToolchainIdentityTest(unittest.TestCase):
    def test_manifest_tracks_contents_executable_mode_and_symlink_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "compiler"
            executable.write_text("compiler", encoding="utf-8")
            executable.chmod(0o755)
            link = root / "gcc"
            link.symlink_to("compiler")

            original = toolchain_identity.directory_manifest_sha256(root)
            executable.chmod(0o644)
            mode_changed = toolchain_identity.directory_manifest_sha256(root)
            executable.chmod(0o755)
            link.unlink()
            link.symlink_to("other")
            link_changed = toolchain_identity.directory_manifest_sha256(root)

            self.assertNotEqual(original, mode_changed)
            self.assertNotEqual(original, link_changed)

    def test_cli_accepts_matching_digest_and_rejects_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "compiler").write_text("compiler", encoding="utf-8")
            expected = toolchain_identity.directory_manifest_sha256(root)
            command = [
                sys.executable,
                str(Path(toolchain_identity.__file__).resolve()),
                "--directory",
                str(root),
                "--expected",
            ]

            matched = subprocess.run(
                command + [expected],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            mismatched = subprocess.run(
                command + ["0" * 64],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            self.assertEqual(matched.returncode, 0, matched.stderr)
            self.assertNotEqual(mismatched.returncode, 0)
            self.assertIn("content SHA-256 mismatch", mismatched.stderr)

    @unittest.skipIf(os.name == "nt", "FIFO creation is POSIX-only")
    def test_manifest_rejects_unsupported_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            os.mkfifo(root / "fifo")
            with self.assertRaisesRegex(
                toolchain_identity.ToolchainIdentityError,
                "unsupported toolchain entry",
            ):
                toolchain_identity.directory_manifest_sha256(root)


if __name__ == "__main__":
    unittest.main()
