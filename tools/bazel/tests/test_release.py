from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = ROOT / "scripts/bazel/bazel-release.py"
SPEC = importlib.util.spec_from_file_location("h2_release", MODULE_PATH)
assert SPEC and SPEC.loader
release = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = release
SPEC.loader.exec_module(release)


class ReleaseTest(unittest.TestCase):
    def final_inputs(self, root: Path) -> list[Path]:
        asset = root / "board.update.tar.zlib"
        asset.write_bytes(b"firmware")
        index = root / "firmware-index.json"
        index.write_text(
            json.dumps({
                "format": 1,
                "version": "1.2.3",
                "firmware_count": 1,
                "firmware": [{
                    "platform": "esp",
                    "version": "1.2.3",
                    "assets": [{"name": asset.name}],
                }],
            }),
            encoding="utf-8",
        )
        checksums = root / "SHA256SUMS"
        checksums.write_text(
            f"{release.sha256(asset)}  {asset.name}\n"
            f"{release.sha256(index)}  {index.name}\n",
            encoding="ascii",
        )
        return [asset, index, checksums]

    def test_retired_desktop_slice_is_not_registered(self):
        self.assertNotIn("desktop-" + "macos-arm64", release.SLICES)

    def test_version_is_one_to_three_numeric_components(self):
        for value in ("1", "1.2", "1.2.3"):
            release.validate_version(value)
        for value in ("", "v1", "1.2.3.4", "1.beta"):
            with self.subTest(value=value), self.assertRaises(
                release.ReleaseError
            ):
                release.validate_version(value)

    def test_relative_output_must_stay_below_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                release.resolve_output(root, "catalog", None),
                (root / "build/release/catalog").resolve(),
            )
            with self.assertRaisesRegex(release.ReleaseError, "below build"):
                release.resolve_output(root, "catalog", Path("../escape"))

    def test_absolute_output_requires_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(release.os.environ, {}, clear=True):
                with self.assertRaisesRegex(
                    release.ReleaseError, "GitHub Actions"
                ):
                    release.resolve_output(
                        Path.cwd(), "catalog", Path(directory)
                    )

    def test_input_rejects_symlink_and_duplicate_basename(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target"
            target.write_text("data", encoding="utf-8")
            (root / "link").symlink_to(target)
            with self.assertRaisesRegex(release.ReleaseError, "symlink"):
                release.input_files(root)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a").mkdir()
            (root / "b").mkdir()
            (root / "a/file").write_text("a", encoding="utf-8")
            (root / "b/file").write_text("b", encoding="utf-8")
            with self.assertRaisesRegex(release.ReleaseError, "duplicate"):
                release.input_files(root)

    def test_catalog_rejects_bk3633(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware-catalog.json"
            path.write_text(
                '[{"entry":"e","label":"//e:firmware","platform":"bk3633",'
                '"target":"bk3633","version":"1.2.3"}]',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(release.ReleaseError, "invalid entry"):
                release.load_catalog([path], "1.2.3")

    def test_catalog_requires_final_package_target(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware-catalog.json"
            path.write_text(
                '[{"entry":"e","label":"//e:firmware","platform":"esp",'
                '"target":"esp32s3","version":"1.2.3"}]',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(release.ReleaseError, "invalid entry"):
                release.load_catalog([path], "1.2.3")

    def test_catalog_rejects_starlark_errors_with_zero_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            result = subprocess.CompletedProcess(
                args=["bazel", "cquery"],
                returncode=0,
                stdout="",
                stderr="ERROR: Starlark evaluation error",
            )
            discovery = subprocess.CompletedProcess(
                args=["bazel", "query"],
                returncode=0,
                stdout="//projects/example/s3:package\n",
                stderr="",
            )
            with mock.patch.object(
                release, "command", side_effect=[discovery, result]
            ):
                with self.assertRaisesRegex(release.ReleaseError, "reported an error"):
                    release.build_catalog(ROOT, "bazel", "1.2.3", output)

    def test_catalog_queries_each_releasable_firmware_configuration(self):
        entries = {
            "esp32s3": {
                "board": "devkit",
                "entry": "projects/example/s3",
                "image": "example",
                "label": "//projects/example/s3:package",
                "platform": "esp",
                "role": "app",
                "target": "esp32s3",
                "version": "1.2.3",
            },
            "esp32p4": {
                "board": "p4",
                "entry": "projects/example/p4",
                "image": "example",
                "label": "//projects/example/p4:package",
                "platform": "esp",
                "role": "app",
                "target": "esp32p4",
                "version": "1.2.3",
            },
            "bk7258": {
                "board": "bk",
                "entry": "projects/example/bk",
                "image": "example",
                "label": "//projects/example/bk:package",
                "platform": "bk7258",
                "role": "app",
                "target": "bk7258",
                "version": "1.2.3",
            },
        }
        results = [
            subprocess.CompletedProcess(
                args=["bazel", "query"],
                returncode=0,
                stdout="\n".join(
                    entry["label"] for entry in entries.values()
                ) + "\n",
                stderr="",
            ),
            *[
                subprocess.CompletedProcess(
                    args=["bazel", "cquery"],
                    returncode=0,
                    stdout=json.dumps(entries[config]) + "\n",
                    stderr="",
                )
                for config in release.CATALOG_CONFIGS
            ],
        ]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            with mock.patch.object(
                release, "command", side_effect=results
            ) as command:
                release.build_catalog(ROOT, "bazel", "1.2.3", output)
            self.assertEqual(command.call_count, 4)
            discovery = command.call_args_list[0]
            self.assertEqual(discovery.args[1][1], "query")
            for config, invocation in zip(
                release.CATALOG_CONFIGS, command.call_args_list[1:]
            ):
                self.assertIn(f"--config={config}", invocation.args[1])
                expression = invocation.args[1][4]
                self.assertTrue(expression.startswith("set("), expression)
                for entry in entries.values():
                    self.assertIn(entry["label"], expression)
            catalog = json.loads(
                (output / "firmware-catalog.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                [item["target"] for item in catalog],
                ["bk7258", "esp32p4", "esp32s3"],
            )

    def test_s3_release_keeps_wifi_credentials_out_of_argv(self):
        credentials = '{"ssid":"fixture","password":"fixture"}'
        catalog = {
            "board": "devkit",
            "entry": "projects/example/s3",
            "image": "example",
            "label": "//projects/example/s3:package",
            "platform": "esp",
            "role": "app",
            "target": "esp32s3",
            "version": "1.2.3",
        }
        build = subprocess.CompletedProcess(
            args=["bazel", "build"], returncode=0, stdout="", stderr=""
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog_path = root / "firmware-catalog.json"
            catalog_path.write_text(json.dumps([catalog]), encoding="utf-8")
            (root / "asset").write_text("firmware", encoding="utf-8")
            query = subprocess.CompletedProcess(
                args=["bazel", "cquery"],
                returncode=0,
                stdout=f"{root / 'asset'}\n",
                stderr="",
            )
            output = root / "output"
            output.mkdir()
            with mock.patch.dict(
                release.os.environ,
                {"H2LOADER_WIFI_CREDENTIALS": credentials},
                clear=True,
            ), mock.patch.object(
                release, "command", side_effect=[build, query]
            ) as command:
                release.build_firmware(
                    root,
                    "bazel",
                    "esp32s3",
                    "1.2.3",
                    [catalog_path],
                    output,
                )
            arguments = command.call_args_list[0].args[1]
            self.assertNotIn(credentials, " ".join(arguments))
            self.assertTrue(arguments[1].startswith("--bazelrc="), arguments)
            self.assertEqual(arguments[2:4], ["build", "--noannounce_rc"])
            self.assertFalse(Path(arguments[1].partition("=")[2]).exists())

    def test_final_bundle_replaces_partial_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_dir = root / "input"
            output = root / "output"
            input_dir.mkdir()
            output.mkdir()
            self.final_inputs(input_dir)
            release.assemble_final(
                list(release.input_files(input_dir)),
                output,
                "1.2.3",
            )
            checksums = (output / "SHA256SUMS").read_text(encoding="ascii")
            self.assertIn("firmware-index.json", checksums)
            self.assertIn("board.update.tar.zlib", checksums)
            self.assertNotIn("SHA256SUMS\n", checksums)

    def test_final_bundle_rejects_unexpected_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.final_inputs(root)
            unexpected = root / "debug.log"
            unexpected.write_text("private output", encoding="utf-8")
            with self.assertRaisesRegex(
                release.ReleaseError,
                "unexpected=.*debug.log",
            ):
                release.assemble_final(
                    [*files, unexpected],
                    root / "output",
                    "1.2.3",
                )

    def test_final_bundle_rejects_checksum_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.final_inputs(root)
            (root / "board.update.tar.zlib").write_bytes(b"tampered")
            with self.assertRaisesRegex(
                release.ReleaseError,
                "checksum mismatch: board.update.tar.zlib",
            ):
                release.assemble_final(files, root / "output", "1.2.3")

    def test_final_bundle_rejects_bk3633_and_unexpected_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            index = root / "firmware-index.json"
            index.write_text(
                json.dumps({
                    "format": 1,
                    "version": "1.2.3",
                    "firmware_count": 1,
                    "firmware": [{
                        "platform": "bk3633",
                        "version": "1.2.3",
                        "assets": [],
                    }],
                }),
                encoding="utf-8",
            )
            checksums = root / "SHA256SUMS"
            checksums.write_text(
                f"{release.sha256(index)}  {index.name}\n",
                encoding="ascii",
            )
            with self.assertRaisesRegex(
                release.ReleaseError,
                "invalid entry",
            ):
                release.assemble_final(
                    [index, checksums],
                    root / "output",
                    "1.2.3",
                )

    def test_catalog_slice_rejects_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(
                release.ReleaseError, "does not accept"
            ):
                release.run_slice(
                    root,
                    "bazel",
                    "catalog",
                    "1.2.3",
                    root,
                    root / "output",
                )


if __name__ == "__main__":
    unittest.main()
