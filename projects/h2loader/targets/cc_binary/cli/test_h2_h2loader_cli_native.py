import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zlib

BINARY = Path(sys.argv[1]).resolve()
FAILURE_BINARY = Path(sys.argv[2]).resolve()
PAYLOAD = bytes([0xE9, 1, 2, 3, 4, 5])


class NativeCliTest(unittest.TestCase):
    def run_cli(self, *arguments, binary=BINARY, cwd=None, env=None, hidden=True):
        environment = os.environ.copy()
        environment.pop("BUILD_WORKING_DIRECTORY", None)
        if env:
            environment.update(env)
        return subprocess.run(
            [str(binary), "--no-ble", *map(str, arguments)],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            encoding="utf-8",
            cwd=cwd,
            env=environment,
            timeout=30,
            check=False,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" and hidden else 0,
        )

    def test_help_starts_with_piped_stdio(self):
        for hidden in [False, True]:
            with self.subTest(hidden=hidden):
                result = self.run_cli("--help", hidden=hidden)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("usage: h2loader", result.stdout)

    def test_initialization_failure_reports_stage_and_exit(self):
        result = self.run_cli("--help", binary=FAILURE_BINARY)
        self.assertEqual(result.returncode, 3)
        self.assertEqual(result.stdout, "")
        self.assertIn("native platform initialization failed code=", result.stderr)

    def create_package(self, image, output, *, cwd=None, env=None):
        result = self.run_cli(
            "package", "--app-bin", image, "--out", output,
            "--board", "fixture", "--target", "host", "--version", "0.1.0",
            cwd=cwd, env=env,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("H2_LOADER_PACKAGE result=OK", result.stdout)

    def test_native_file_arguments_and_image_bytes(self):
        with tempfile.TemporaryDirectory(dir=None if os.name == "nt" else "/tmp") as temporary:
            folder = Path(temporary) / "子目录"
            folder.mkdir()
            image = folder / "固件.bin"
            image.write_bytes(PAYLOAD)
            absolute = folder / "absolute.tar"
            self.create_package(image, absolute)
            relative = folder / "relative.tar"
            self.create_package(image.name, relative.name, cwd=folder)
            invocation = folder / "invocation.tar"
            self.create_package(image.name, invocation.name, cwd=temporary,
                                env={"BUILD_WORKING_DIRECTORY": str(folder)})
            for output in [absolute, relative, invocation]:
                with tarfile.open(output, "r:") as archive:
                    self.assertEqual(zlib.decompress(archive.extractfile("app.bin.zlib").read()), PAYLOAD)
            if os.name == "nt":
                extended = folder / "extended.tar"
                self.create_package("\\\\?\\" + str(image), "\\\\?\\" + str(extended))
                portable = folder / "portable.tar"
                image_portable = "/" + str(image)[0].lower() + str(image)[2:].replace("\\", "/")
                output_portable = "/" + str(portable)[0].lower() + str(portable)[2:].replace("\\", "/")
                self.create_package(image_portable, output_portable)
                for output in [extended, portable]:
                    with tarfile.open(output, "r:") as archive:
                        self.assertEqual(zlib.decompress(archive.extractfile("app.bin.zlib").read()), PAYLOAD)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
