"""Check native input assembly and fail-closed status/config validation."""

import io
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

from tools.bazel.native.rockchip import runner


class NativeInputsTest(unittest.TestCase):
    def test_invocation(self):
        runner.check_invocation("OTHER_STATUS 1\nSTABLE_ROCKCHIP_BUILD_ID d549d330-bc48-454e-8a92-5aabdb1ec872\n")
        for invalid in ("", "STABLE_ROCKCHIP_BUILD_ID fixed", "STABLE_ROCKCHIP_BUILD_ID d549d330"):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "workspace_status_command"):
                runner.check_invocation(invalid)

    def test_relative_install_paths_and_modes(self):
        with tempfile.TemporaryDirectory() as scratch:
            work = Path(scratch)
            root = work / "fs"
            root.mkdir()
            source = work / "payload"
            source.write_bytes(b"ELF fixture")
            runner.install(root, [(source, "usr/bin/app")], 0o755)
            target = root / "usr/bin/app"
            self.assertEqual(target.read_bytes(), b"ELF fixture")
            self.assertEqual(target.stat().st_mode & 0o777, 0o755)
            with self.assertRaisesRegex(ValueError, "existing destination"):
                runner.install(root, [(source, "usr/bin/app")], 0o644)
            (root / "outside").symlink_to(work)
            for invalid in ("", "/bin/app", "usr/../bin/app", "usr//app", "outside/payload"):
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    runner.install(root, [(source, invalid)], 0o755)
            self.assertEqual(source.read_bytes(), b"ELF fixture")

    @unittest.skipUnless(sys.platform.startswith("linux"), "native Linux stage")
    def test_kernel_stage_uses_explicit_target_and_ordered_patches(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            work = root / "build"
            work.mkdir()
            files = {
                "Makefile": (
                    "olddefconfig:\n\t@true\n"
                    "Image:\n\t@cp version arch/arm64/boot/Image\n"
                    "vendor/console.dtb:\n\t@cp arch/arm64/boot/dts/vendor/console.dts "
                    "arch/arm64/boot/dts/vendor/console.dtb\n"
                ),
                "arch/arm64/boot/dts/vendor/console.dts": "upstream",
                "version": "base\n",
            }
            with tarfile.open(root / "source.tar", "w") as archive:
                for name, contents in files.items():
                    payload = contents.encode()
                    entry = tarfile.TarInfo("upstream/" + name)
                    entry.size = len(payload)
                    archive.addfile(entry, io.BytesIO(payload))
            for name, before, after in (("first", "base", "intermediate"), ("second", "intermediate", "final")):
                (root / name).write_text(f"--- a/version\n+++ b/version\n@@ -1 +1 @@\n-{before}\n+{after}\n")
            (root / "config").write_text("CONFIG_TEST=y\n")
            (root / "consumer.dts").write_bytes(b"consumer board")
            stage = runner.Stage(root, work, {"PATH": os.defpath, "SOURCE_DATE_EPOCH": "1704067200"})
            image, dtb = runner.linux_kernel(stage, {
                "archive": "source.tar", "patches": ["first", "second"], "config": "config",
                "dts_files": [["consumer.dts", "vendor/console.dts"]], "dtb_target": "vendor/console.dtb",
            })
            self.assertEqual(image.read_bytes(), b"final\n")
            self.assertEqual(dtb.read_bytes(), b"consumer board")

    def test_ignored_config_key_fails(self):
        with tempfile.TemporaryDirectory() as scratch:
            config = Path(scratch) / ".config"
            config.write_text("CONFIG_MMC=y\n# CONFIG_SOUND is not set\n")
            runner.check_config(["CONFIG_MMC=y"], config)
            with self.assertRaisesRegex(ValueError, "CONFIG_MISSING"):
                runner.check_config(["CONFIG_MMC=y", "CONFIG_MISSING=y"], config)


if __name__ == "__main__":
    unittest.main()
