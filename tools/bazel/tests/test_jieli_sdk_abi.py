"""Exercise the shared layout's fail-closed linker assertions with GNU ld."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/sdk_abi.ld"


@unittest.skipUnless(platform.system() == "Linux", "requires the native Linux GNU linker")
class SdkAbiTest(unittest.TestCase):
    def test_missing_public_accept_definition_fails_link(self):
        symbols = ("os_mutex_accept", "os_sem_accept")
        with tempfile.TemporaryDirectory(prefix="h2-sdk-abi-") as directory:
            root = Path(directory)
            for missing in (None, *symbols):
                with self.subTest(missing=missing):
                    source = root / "input.c"
                    source.write_text("\n".join(
                        f"int {symbol}(void *p) {{ return p != 0; }}"
                        for symbol in symbols if symbol != missing
                    ), encoding="utf-8")
                    obj = root / "input.o"
                    subprocess.run(["cc", "-c", str(source), "-o", str(obj)],
                                   check=True, timeout=30)
                    result = subprocess.run(
                        ["ld", "-r", "-T", str(SCRIPT), str(obj), "-o", str(root / "out.o")],
                        capture_output=True, text=True, timeout=30)
                    if missing is None:
                        self.assertEqual(result.returncode, 0, result.stderr)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn(f"WL82 SDK missing {missing}", result.stderr)


if __name__ == "__main__":
    unittest.main()
