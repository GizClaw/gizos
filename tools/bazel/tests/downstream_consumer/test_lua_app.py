"""Check real public cquery receipts and every downstream Lua app output."""

import hashlib
import json
from pathlib import Path
import sys
import tarfile
import unittest

CATALOG, RELEASE_FILES, DEFAULT_FILES, *OUTPUTS = [Path(path) for path in sys.argv[1:]]
sys.argv[1:] = []


class LuaAppReleaseTest(unittest.TestCase):
    def test_provider_discovery_and_all_output_groups(self):
        catalog_rows = [json.loads(line) for line in CATALOG.read_text().splitlines()]
        release_rows = [json.loads(line) for line in RELEASE_FILES.read_text().splitlines()]
        self.assertEqual(len(catalog_rows), 2)
        self.assertEqual(len(release_rows), 2)
        catalog = {row["app_id"]: row for row in catalog_rows}
        release_files = {Path(row["archive"]).name: row for row in release_rows}
        expected = (
            ("abc", "1.2.3", None, False),
            ("xyz", "2.0.0-rc.1", "xyz", True),
        )
        self.assertEqual(set(catalog), {"abc", "xyz"})
        self.assertEqual(len(OUTPUTS), 6)
        default_files = DEFAULT_FILES.read_text().splitlines()
        self.assertEqual(len(default_files), 6)
        queried_outputs = []
        for index, (app_id, version, data_dir, compact) in enumerate(expected):
            with self.subTest(app_id=app_id):
                entry = app_id + ".lua"
                identity = {
                    "app_id": app_id,
                    "version": version,
                    "entry": entry,
                    "data_dir": data_dir,
                    "compact": compact,
                }
                self.assertEqual(
                    catalog[app_id],
                    {"label": "//:" + app_id + "_lua_app", **identity},
                )
                archive, manifest, metadata = OUTPUTS[index * 3:index * 3 + 3]
                stem = app_id + "-" + version + ".lua-app"
                self.assertEqual(archive.name, stem + ".tar.gz")
                self.assertEqual(manifest.name, stem + ".manifest.json")
                self.assertEqual(metadata.name, stem + ".json")
                structured = release_files[archive.name]
                self.assertEqual(set(structured), {"archive", "manifest", "metadata"})
                for field, output in zip(("archive", "manifest", "metadata"),
                                         (archive, manifest, metadata)):
                    queried = structured[field]
                    self.assertTrue(output.resolve().as_posix().endswith("/" + queried))
                    queried_outputs.append(queried)
                value = json.loads(manifest.read_text())
                self.assertEqual(value["format"], 1)
                self.assertEqual(value["type"], "lua-app")
                for field, wanted in identity.items():
                    self.assertEqual(value[field], wanted)
                release = json.loads(metadata.read_text())
                self.assertEqual(release["manifest"], value)
                for field in ("format", "type", *identity):
                    self.assertEqual(release[field], value[field])
                self.assertEqual(release["package"], {
                    "name": archive.name,
                    "size": archive.stat().st_size,
                    "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                })
                with tarfile.open(archive) as package:
                    self.assertEqual(package.extractfile("manifest.json").read(),
                                     manifest.read_bytes())
                    self.assertEqual(set(package.getnames()),
                                     {"manifest.json", *(file["path"] for file in value["files"])})
                    for file in value["files"]:
                        payload = package.extractfile(file["path"]).read()
                        self.assertEqual(file["size"], len(payload))
                        self.assertEqual(file["sha256"], hashlib.sha256(payload).hexdigest())
        self.assertEqual(set(default_files), set(queried_outputs))


if __name__ == "__main__":
    unittest.main()
