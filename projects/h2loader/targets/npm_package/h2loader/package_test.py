#!/usr/bin/env python3
"""Validates the publishable H2Loader npm package directory."""

import json
from pathlib import Path
import re
import sys
import unittest


EXPECTED_FILES = {
    "LICENSE",
    "README.md",
    "h2loader.js",
    "h2loader.wasm",
    "h2loader_runtime.js",
    "package.json",
}


class PackageTest(unittest.TestCase):
    def test_package_contract(self):
        package_root = Path(sys.argv[1])
        actual_files = {
            str(path.relative_to(package_root))
            for path in package_root.rglob("*")
            if path.is_file()
        }
        self.assertEqual(actual_files, EXPECTED_FILES)

        manifest = json.loads((package_root / "package.json").read_text())
        self.assertEqual(manifest["name"], "@gizclaw/h2loader")
        self.assertRegex(
            manifest["version"],
            re.compile(
                r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
                r"(?:-[0-9A-Za-z.-]+)?$",
            ),
        )
        self.assertEqual(manifest["type"], "module")
        self.assertEqual(manifest["exports"], {".": "./h2loader.js"})
        self.assertEqual(manifest["publishConfig"], {
            "access": "public",
            "registry": "https://npm.pkg.github.com",
        })
        self.assertEqual(set(manifest["files"]), EXPECTED_FILES - {"package.json"})

        entry = (package_root / "h2loader.js").read_text()
        self.assertIn('from "./h2loader_runtime.js"', entry)
        self.assertIn('new URL("./h2loader.wasm", import.meta.url)', entry)
        self.assertGreater((package_root / "h2loader_runtime.js").stat().st_size, 0)
        self.assertEqual((package_root / "h2loader.wasm").read_bytes()[:4], b"\x00asm")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
