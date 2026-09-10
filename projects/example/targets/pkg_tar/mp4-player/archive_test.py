import pathlib
import sys
import tarfile
import unittest


class WebArchiveTest(unittest.TestCase):
    def test_archive(self):
        with tarfile.open(pathlib.Path(sys.argv[1])) as archive:
            names = [member.name for member in archive.getmembers()]
            self.assertEqual(["index.html", "index.js", "index.wasm"], names)
            for member in archive.getmembers():
                path = pathlib.PurePosixPath(member.name)
                self.assertFalse(path.is_absolute())
                self.assertNotIn("..", path.parts)
                self.assertTrue(member.isfile())
                self.assertEqual(0o644, member.mode)
            html = archive.extractfile("index.html").read().decode("utf-8")
            javascript = archive.extractfile("index.js").read().decode("utf-8")
            wasm = archive.extractfile("index.wasm").read(4)
            self.assertIn('id="start"', html)
            template_start = html.index('<template id="program">')
            loader_start = html.index('src="index.js"')
            template_end = html.index("</template>", template_start)
            self.assertLess(template_start, loader_start)
            self.assertLess(loader_start, template_end)
            self.assertIn("start.addEventListener('click'", html)
            self.assertIn("emscripten_fs_load_embedded_files", javascript)
            self.assertEqual(b"\x00asm", wasm)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
