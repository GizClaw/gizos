from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.bazel import native_ccache


class NativeCcacheTest(unittest.TestCase):
    def _write_executable(self, path: Path, contents: str) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
        path.chmod(0o755)
        return path

    def _write_runtime(
        self,
        root: Path,
        *,
        remote: bool = False,
        enabled: bool = True,
    ) -> Path:
        locator = root / "locator.json"
        if not enabled:
            locator.write_text(
                json.dumps({
                    "schema": "h2.native-locator.v1",
                    "kind": "native-ccache-runtime",
                    "enabled": False,
                    "paths": {},
                    "metadata": {},
                }),
                encoding="utf-8",
            )
            return locator
        self._write_executable(
            root / "bin/ccache",
            "#!/bin/sh\nprintf 'ccache version 4.13.6\\n'\n",
        )
        runtime = {
            "schema": native_ccache.RUNTIME_SCHEMA,
            "ccache": "bin/ccache",
            "cache_root": "cache",
        }
        if remote:
            self._write_executable(
                root / "bin/ccache-storage-https",
                "#!/bin/sh\nexit 0\n",
            )
            token = root / "token"
            token.write_text("secret-token\n", encoding="utf-8")
            token.chmod(0o600)
            runtime.update({
                "remote_base_url": "https://storage.googleapis.com/build-cache/ccache",
                "storage_helper": "bin/ccache-storage-https",
                "token_file": "token",
            })
        manifest = root / "runtime.json"
        manifest.write_text(json.dumps(runtime), encoding="utf-8")
        locator.write_text(
            json.dumps({
                "schema": "h2.native-locator.v1",
                "kind": "native-ccache-runtime",
                "enabled": True,
                "paths": {"root": str(root), "manifest": str(manifest)},
                "metadata": {},
            }),
            encoding="utf-8",
        )
        return locator

    def test_disabled_locator_preserves_uncached_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            locator = self._write_runtime(root, enabled=False)
            self.assertIsNone(
                native_ccache.configure_environment(
                    {"PATH": "/usr/bin:/bin"},
                    "esp32s3",
                    root / "action",
                    str(locator),
                )
            )

    def test_configure_environment_uses_target_namespace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            locator = self._write_runtime(root)
            environment = {"PATH": "/usr/bin:/bin"}
            result = native_ccache.configure_environment(
                environment, "esp32s3", root / "action", str(locator)
            )
            self.assertEqual(result, root.resolve() / "bin/ccache")
            self.assertEqual(
                environment["CCACHE_DIR"],
                str(root.resolve() / "cache/esp32s3"),
            )
            self.assertEqual(environment["CCACHE_BASEDIR"], str(root / "action"))
            self.assertEqual(environment["CCACHE_COMPILERCHECK"], "content")
            self.assertEqual(environment["CCACHE_MAXSIZE"], "1GiB")
            self.assertEqual(environment["CCACHE_NOHASHDIR"], "1")
            self.assertNotIn("CCACHE_REMOTE_STORAGE", environment)

    def test_remote_configuration_uses_family_prefix_without_logging_token(self):
        for namespace, family in (
            ("esp32s3", "esp"),
            ("esp32p4", "esp"),
            ("esp32c5", "esp"),
            ("bk7258", "bk"),
            ("bk3633", "bk"),
        ):
            with (
                self.subTest(namespace=namespace),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary)
                locator = self._write_runtime(root, remote=True)
                environment = {"PATH": "/usr/bin:/bin"}
                with mock.patch("builtins.print") as print_mock:
                    native_ccache.configure_environment(
                        environment, namespace, root / "action", str(locator)
                    )
                remote = environment["CCACHE_REMOTE_STORAGE"]
                self.assertTrue(
                    remote.startswith(
                        "https://storage.googleapis.com/"
                        f"build-cache/ccache/{family} "
                    )
                )
                self.assertIn("@layout=subdirs", remote)
                self.assertEqual(environment["CCACHE_RESHARE"], "1")
                self.assertIn("@bearer-token=secret-token", remote)
                output = " ".join(
                    str(argument) for call in print_mock.call_args_list for argument in call.args
                )
                self.assertNotIn("secret-token", output)

    def test_runtime_paths_cannot_escape_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            locator = self._write_runtime(root)
            runtime = root / "runtime.json"
            document = json.loads(runtime.read_text(encoding="utf-8"))
            document["ccache"] = "../ccache"
            runtime.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(
                native_ccache.NativeCcacheError,
                "escapes the runtime root",
            ):
                native_ccache.configure_environment(
                    {"PATH": "/usr/bin:/bin"}, "esp32s3", root / "action", str(locator)
                )

    def test_remote_configuration_rejects_invalid_url(self):
        for value in (
            "http://storage.googleapis.com/cache/ccache",
            "https://example.com/cache/ccache",
            "https://storage.googleapis.com/cache/firmwares",
            "https://storage.googleapis.com/cache/ccache/esp",
            "https://storage.googleapis.com/cache/ccache?token=value",
        ):
            with self.subTest(value=value):
                with self.assertRaisesRegex(native_ccache.NativeCcacheError, "remote_base_url"):
                    native_ccache._remote_endpoint(value, "esp")

    def test_remote_configuration_requires_private_token_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            token_file = Path(temporary) / "token"
            token_file.write_text("secret-token", encoding="utf-8")
            token_file.chmod(0o644)
            with self.assertRaisesRegex(native_ccache.NativeCcacheError, "mode 0600"):
                native_ccache._read_token_file(str(token_file))

    def test_remote_configuration_requires_pinned_ccache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            locator = self._write_runtime(root, remote=True)
            self._write_executable(
                root / "bin/ccache",
                "#!/bin/sh\nprintf 'ccache version 4.12.3\\n'\n",
            )

            with self.assertRaisesRegex(
                native_ccache.NativeCcacheError,
                "must be version 4.13.6",
            ):
                native_ccache.configure_environment(
                    {"PATH": "/usr/bin:/bin"},
                    "esp32s3",
                    root / "action",
                    str(locator),
                )

    def test_wrapped_toolchain_delegates_compilers_to_ccache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            toolchain = root / "toolchain"
            toolchain.mkdir()
            for name in ("arm-none-eabi-gcc", "arm-none-eabi-ar"):
                self._write_executable(
                    toolchain / name,
                    "#!/bin/sh\nprintf 'compiler:%s\\n' \"$*\"\n",
                )
            ccache = self._write_executable(
                root / "ccache",
                "#!/bin/sh\nexec \"$@\"\n",
            )
            wrapper = native_ccache.create_wrapped_toolchain(
                toolchain, root / "wrapper", ccache
            )
            result = subprocess.run(
                [str(wrapper / "arm-none-eabi-gcc"), "-c", "fixture.c"],
                check=True,
                stdout=subprocess.PIPE,
                text=True,
            )
            self.assertEqual(result.stdout, "compiler:-c fixture.c\n")
            self.assertTrue((wrapper / "arm-none-eabi-ar").is_symlink())


if __name__ == "__main__":
    unittest.main()
