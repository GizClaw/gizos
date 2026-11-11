"""Exercise the production BK TLS lifetime coordinator with native barriers."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[3]


class TLSLifetime(unittest.TestCase):
    def test_bounded_return_and_socket_ownership(self):
        compiler = shutil.which("cc")
        self.assertTrue(compiler, "a native C compiler is required")
        with tempfile.TemporaryDirectory(prefix="h2-bk-tls-lifetime-") as directory:
            binary = Path(directory) / "lifetime"
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-pthread", "-I", str(REPO / "pal/include"),
                 "-I", str(PACKAGE / "src"),
                 str(PACKAGE / "src/h2_bk_tls_lifetime.c"),
                 str(PACKAGE / "tests/test_h2_bk_tls_lifetime.c"),
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("coordinator regressions passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
