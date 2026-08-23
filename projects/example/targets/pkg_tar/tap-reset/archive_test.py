import pathlib
import sys
import tarfile
import unittest


class WebArchiveTest(unittest.TestCase):
    def setUp(self):
        self.archive_path = pathlib.Path(sys.argv[1])

    def test_archive_is_a_safe_servable_root(self):
        with tarfile.open(self.archive_path) as archive:
            members = archive.getmembers()
            self.assertEqual(
                ["index.html", "index.js", "index.wasm"],
                [member.name for member in members],
            )
            for member in members:
                path = pathlib.PurePosixPath(member.name)
                self.assertFalse(path.is_absolute())
                self.assertNotIn("..", path.parts)
                self.assertTrue(member.isfile())
                self.assertEqual(0o644, member.mode)

    def test_entrypoint_references_packaged_outputs(self):
        with tarfile.open(self.archive_path) as archive:
            html = archive.extractfile("index.html").read().decode("utf-8")
            javascript = archive.extractfile("index.js").read().decode("utf-8")
            wasm = archive.extractfile("index.wasm").read(4)

        self.assertIn('src="index.js"', html)
        self.assertIn("index.wasm", javascript)
        self.assertEqual(b"\x00asm", wasm)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
