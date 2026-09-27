import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest


class ReleaseTarballTest(unittest.TestCase):
    def test_built_tarball_has_exact_files_and_normalized_metadata(self):
        root = (
            Path(os.environ["TEST_SRCDIR"])
            / os.environ["TEST_WORKSPACE"]
            / "projects/h2loader/targets/npm_package/h2loader"
        )
        manifest = json.loads((root / "package.json").read_text())
        basename = manifest["name"].removeprefix("@").replace("/", "-")
        tarball = root / "release_tarball" / f"{basename}-{manifest['version']}.tgz"
        self.assertEqual(list(tarball.parent.iterdir()), [tarball])
        header = tarball.read_bytes()[:10]
        self.assertEqual(header[:8], b"\x1f\x8b\x08\x00\x00\x00\x00\x00")
        expected = {f"package/{name}" for name in [*manifest["files"], "package.json"]}
        with tarfile.open(tarball) as archive:
            members = archive.getmembers()
            names = [item.name for item in members]
            self.assertEqual(names, sorted(set(names)))
            self.assertEqual({item.name for item in members if item.isfile()}, expected)
            self.assertEqual({item.name for item in members if item.isdir()}, {"package"})
            for item in members:
                self.assertTrue(item.isdir() or item.isfile(), item.name)
                self.assertEqual((item.mtime, item.uid, item.gid, item.uname, item.gname), (0, 0, 0, "", ""))
                self.assertEqual(item.mode, 0o755 if item.isdir() else 0o644)
                self.assertEqual(item.pax_headers, {})
            with tempfile.TemporaryDirectory() as directory:
                archive.extractall(directory, filter="data")
                extracted = Path(directory)
                self.assertEqual([path.name for path in extracted.iterdir()], ["package"])
                self.assertEqual({path.relative_to(extracted).as_posix() for path in extracted.rglob("*") if path.is_file()}, expected)
                self.assertEqual((extracted / "package/package.json").read_bytes(), (root / "package.json").read_bytes())


if __name__ == "__main__":
    unittest.main()
