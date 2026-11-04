"""Execute the complete native Image/Power provider against SDK and FS fakes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class ImagePowerTest(unittest.TestCase):
    def test_transaction_and_native_boot_barriers(self):
        source = (ROOT / "native_component_src/jieli/wl82/h2_pal_image/src/h2_jieli_wl82_platform_image.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        fixture = (ROOT / "tools/bazel/tests/fixtures/jieli_image_power.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            unit.write_text(fixture.replace("/* PROVIDER */", source))
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")),
                "-I", str(ROOT / "libs/pal/include"),
                "-I", str(ROOT / "libs/atomic/include"),
                "-I", str(ROOT / "native_component_src/jieli/wl82/h2_pal_image/include"),
                str(unit), str(ROOT / "libs/atomic/providers/c11/src/h2_atomic_c11.c"),
                "-o", str(binary)], check=True, timeout=60)
            for case in ("stored", "readonly", "trial", "recovery", "late", "cleanup", "failed_mapping", "short_tail", "small"):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
