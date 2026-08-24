from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

from tools.bazel import native_runtime


class NativeRuntimeTest(unittest.TestCase):
    def test_bazel_python_runtime_is_pinned(self):
        self.assertEqual(sys.version_info[:2], (3, 11))

    def test_locator_requires_schema_kind_and_absolute_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            locator = root / "locator.json"
            locator.write_text(json.dumps({
                "schema": native_runtime.LOCATOR_SCHEMA,
                "kind": "esp-idf-sdk",
                "enabled": True,
                "paths": {"root": str(root)},
            }), encoding="utf-8")
            document = native_runtime.read_locator(str(locator), "esp-idf-sdk")
            self.assertEqual(
                native_runtime.locator_path(document, "root", "esp-idf-sdk"),
                root.resolve(),
            )
            with self.assertRaisesRegex(native_runtime.NativeRuntimeError, "kind mismatch"):
                native_runtime.read_locator(str(locator), "bk7258-sdk")

    def test_disabled_locator_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            locator = Path(temporary) / "locator.json"
            locator.write_text(json.dumps({
                "schema": native_runtime.LOCATOR_SCHEMA,
                "kind": "bk3633-sdk",
                "enabled": False,
                "paths": {},
            }), encoding="utf-8")
            with self.assertRaisesRegex(native_runtime.NativeRuntimeError, "not configured"):
                native_runtime.read_locator(str(locator), "bk3633-sdk")

    def test_fixed_environment_does_not_inherit_caller_path(self):
        environment = native_runtime.fixed_environment()
        self.assertNotIn("caller-only", environment["PATH"])
        self.assertEqual(environment["TZ"], "UTC")

    def test_external_repository_source_root_uses_bazel_canonical_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            external = Path(temporary) / "external"
            repository = external / "gizos++vendor_repositories+h2_vendor_pixa"
            include = repository / "src/pkgs/c/include"
            include.mkdir(parents=True)
            self.assertEqual(
                native_runtime.find_external_repository_source_root(
                    [include], "h2_vendor_pixa"
                ),
                repository.joinpath("src").resolve(),
            )
            self.assertIsNone(
                native_runtime.find_external_repository_source_root(
                    [include], "h2_vendor_pixelroot32"
                )
            )


if __name__ == "__main__":
    unittest.main()
