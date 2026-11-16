from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import tools.bazel.native.native_runtime as native_runtime


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
                "kind": "jieli-ac695n-sdk",
                "enabled": False,
                "paths": {},
            }), encoding="utf-8")
            with self.assertRaisesRegex(native_runtime.NativeRuntimeError, "not configured"):
                native_runtime.read_locator(str(locator), "jieli-ac695n-sdk")

    def test_fixed_environment_does_not_inherit_caller_path(self):
        with mock.patch.dict(os.environ, {"PATH": "/caller-only"}):
            environment = native_runtime.fixed_environment()
        self.assertNotIn("caller-only", environment["PATH"])
        self.assertEqual(environment["TZ"], "UTC")

    def test_fixed_environment_prefers_explicit_tools_then_managed_commands(self):
        with mock.patch.object(Path, "is_dir", return_value=True):
            environment = native_runtime.fixed_environment([Path("/pinned/bin")])
        self.assertEqual(
            environment["PATH"].split(os.pathsep)[:2],
            ["/pinned/bin", "/opt/gizmoth/bin"],
        )

    @mock.patch.object(native_runtime.platform, "system", return_value="Linux")
    @mock.patch.object(native_runtime.platform, "machine", return_value="aarch64")
    def test_arm64_translation_ignores_ambient_runtime_flags(self, *_):
        with mock.patch.dict(os.environ, {
            "GIZMOTH_X86_64_TRANSLATION": "qemu-binfmt-v1",
            "QEMU_LD_PREFIX": "/caller-only",
        }):
            with self.assertRaisesRegex(native_runtime.NativeRuntimeError, "UNSUPPORTED"):
                native_runtime.linux_x86_64_environment({"metadata": {}})

    @mock.patch.object(native_runtime.platform, "system", return_value="Linux")
    @mock.patch.object(native_runtime.platform, "machine", return_value="arm64")
    def test_arm64_translation_reaches_child_using_declared_locator(self, *_):
        with tempfile.TemporaryDirectory() as temporary:
            locator = {"metadata": {"x86_64_runtime": {
                "kind": "qemu-binfmt-v1", "ld_prefix": temporary,
            }}}
            environment = native_runtime.fixed_environment()
            environment.update(native_runtime.linux_x86_64_environment(locator, locator))
            result = subprocess.run(
                [sys.executable, "-I", "-c", "import os; print(os.environ['QEMU_LD_PREFIX'])"],
                env=environment, check=True, capture_output=True, text=True,
            )
            self.assertEqual(result.stdout.strip(), str(Path(temporary).resolve()))
            self.assertNotIn("GIZMOTH_X86_64_TRANSLATION", environment)

    @mock.patch.object(native_runtime.platform, "system", return_value="Linux")
    @mock.patch.object(native_runtime.platform, "machine", return_value="aarch64")
    def test_arm64_translation_rejects_invalid_or_mismatched_runtime(self, *_):
        with tempfile.TemporaryDirectory() as temporary:
            def locator(prefix=temporary, kind="qemu-binfmt-v1"):
                return {"metadata": {"x86_64_runtime": {"kind": kind, "ld_prefix": prefix}}}
            for invalid in (locator(kind="native"), locator(prefix="relative"),
                            locator(prefix=temporary + "/absent")):
                with self.subTest(locator=invalid), self.assertRaises(native_runtime.NativeRuntimeError):
                    native_runtime.linux_x86_64_environment(invalid)
            other = Path(temporary) / "other"
            other.mkdir()
            with self.assertRaisesRegex(native_runtime.NativeRuntimeError, "different"):
                native_runtime.linux_x86_64_environment(locator(), locator(str(other)))

    def test_other_hosts_do_not_receive_qemu_environment(self):
        for system, machine in (("Darwin", "arm64"), ("Linux", "x86_64")):
            with mock.patch.object(native_runtime.platform, "system", return_value=system), \
                    mock.patch.object(native_runtime.platform, "machine", return_value=machine):
                self.assertEqual(native_runtime.linux_x86_64_environment({"metadata": {}}), {})

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
                    [include], "h2_vendor_unknown"
                )
            )


if __name__ == "__main__":
    unittest.main()
