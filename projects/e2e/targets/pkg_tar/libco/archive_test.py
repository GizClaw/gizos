import pathlib
import sys
import tarfile
import unittest


class LibcoWebArchiveTest(unittest.TestCase):
    def test_archive(self):
        with tarfile.open(pathlib.Path(sys.argv[1])) as archive:
            self.assertEqual(
                ["index.html", "index.js", "index.wasm"],
                [member.name for member in archive.getmembers()],
            )
            html = archive.extractfile("index.html").read().decode("utf-8")
            javascript = archive.extractfile("index.js").read().decode("utf-8")
            wasm = archive.extractfile("index.wasm").read(4)
        self.assertIn('src="index.js"', html)
        self.assertIn("index.wasm", javascript)
        self.assertEqual(b"\x00asm", wasm)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
