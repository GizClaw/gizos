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
                    "version": "2.0.0",
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
        tarball = root / "scope-example-2.3.4.tgz"
        tarball.write_bytes(b"npm tarball fixture")
        npm_index = root / "npm-index.json"
        npm_index.write_text(json.dumps({
            "format": 1,
            "version": "1.2.3",
            "package_count": 1,
            "packages": [{
                "name": "@scope/example",
                "version": "2.3.4",
                "tarball": tarball.name,
                "sha256": release.sha256(tarball),
                "size": tarball.stat().st_size,
            }],
        }), encoding="utf-8")
        return [asset, index, checksums, tarball, npm_index]

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
                release.load_catalog([path])

    def test_catalog_requires_final_package_target(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware-catalog.json"
            path.write_text(
                '[{"entry":"e","label":"//e:firmware","platform":"esp",'
                '"target":"esp32s3","version":"1.2.3"}]',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(release.ReleaseError, "invalid entry"):
                release.load_catalog([path])

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
                "version": "2.0.0",
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
            self.assertEqual(
                discovery.args[1][2],
                'kind("h2loader_tar_zlib rule", //projects/...) '
                'except attr("tags", "no-release", //projects/...)',
            )
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
            self.assertIn("npm-index.json", checksums)
            self.assertIn("scope-example-2.3.4.tgz", checksums)
            self.assertNotIn("SHA256SUMS\n", checksums)
            release.validate_checksums(
                output / "SHA256SUMS",
                {path.name: path for path in output.iterdir()},
                {"firmware-index.json", "board.update.tar.zlib", "npm-index.json", "scope-example-2.3.4.tgz"},
            )

    def test_final_bundle_rejects_missing_npm_index(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.final_inputs(root)
            with self.assertRaisesRegex(release.ReleaseError, "incomplete.*npm-index.json"):
                release.assemble_final([path for path in files if path.name != "npm-index.json"], root / "output", "1.2.3")

    def test_final_slice_merges_separate_artifacts_and_multiple_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = root / "input"
            inputs.mkdir()
            files = self.final_inputs(inputs)
            for path in files:
                artifact = inputs / (
                    "release-npm-bundle"
                    if path.name == "npm-index.json" or path.suffix == ".tgz"
                    else "release-firmware-bundle"
                )
                artifact.mkdir(exist_ok=True)
                path.rename(artifact / path.name)
            npm = inputs / "release-npm-bundle"
            second = npm / "zebra-4.5.6.tgz"
            second.write_bytes(b"second package")
            index_path = npm / "npm-index.json"
            index = json.loads(index_path.read_text())
            index["packages"].append({
                "name": "zebra",
                "version": "4.5.6",
                "tarball": second.name,
                "sha256": release.sha256(second),
                "size": second.stat().st_size,
            })
            index["package_count"] = 2
            index_path.write_text(json.dumps(index))
            output = root / "output"
            release.run_slice(
                root, "unused-bazel", "release-bundle", "1.2.3", inputs, output
            )
            expected = {
                "firmware-index.json", "board.update.tar.zlib", "npm-index.json",
                "scope-example-2.3.4.tgz", "zebra-4.5.6.tgz",
            }
            self.assertEqual({path.name for path in output.iterdir()}, expected | {"SHA256SUMS"})
            release.validate_checksums(
                output / "SHA256SUMS",
                {path.name: path for path in output.iterdir()},
                expected,
            )
            # A second partial checksum file must not overwrite the firmware one.
            (npm / "SHA256SUMS").write_text("unexpected npm checksums\n")
            with self.assertRaisesRegex(release.ReleaseError, "duplicate.*SHA256SUMS"):
                release.run_slice(
                    root, "unused-bazel", "release-bundle", "1.2.3", inputs, output
                )
            # Input rejection happens before clearing a previously valid staging dir.
            self.assertEqual({path.name for path in output.iterdir()}, expected | {"SHA256SUMS"})

    def test_final_bundle_still_requires_exact_firmware_checksum_coverage(self):
        for checksum_asset in ("board.update.tar.zlib", "npm-index.json"):
            with self.subTest(asset=checksum_asset), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                files = self.final_inputs(root)
                checksums = root / "SHA256SUMS"
                lines = checksums.read_text().splitlines(keepends=True)
                if checksum_asset == "board.update.tar.zlib":
                    checksums.write_text("".join(line for line in lines if checksum_asset not in line))
                else:
                    checksums.write_text("".join(lines) + f"{release.sha256(root / checksum_asset)}  {checksum_asset}\n")
                with self.assertRaisesRegex(release.ReleaseError, "checksum coverage differs"):
                    release.assemble_final(files, root / "output", "1.2.3")

    def test_final_bundle_rejects_corrupt_npm_tarball(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.final_inputs(root)
            tarball = root / "scope-example-2.3.4.tgz"
            # Preserve size so this exercises the digest check specifically.
            tarball.write_bytes(b"x" * tarball.stat().st_size)
            with self.assertRaisesRegex(release.ReleaseError, "tarball integrity mismatch"):
                release.assemble_final(files, root / "output", "1.2.3")

    def test_final_bundle_rejects_missing_npm_tarball(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.final_inputs(root)
            with self.assertRaisesRegex(release.ReleaseError, "inputs differ: missing=.*scope-example"):
                release.assemble_final([path for path in files if path.suffix != ".tgz"], root / "output", "1.2.3")

    def test_final_bundle_rejects_invalid_npm_index_identity(self):
        for key, value in (("format", 2), ("format", True), ("version", "2.0.0"), ("package_count", 2), ("package_count", True), ("packages", [])):
            with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                files = self.final_inputs(root)
                path = root / "npm-index.json"
                data = json.loads(path.read_text())
                data[key] = value
                path.write_text(json.dumps(data))
                with self.assertRaisesRegex(release.ReleaseError, "npm index identity"):
                    release.assemble_final(files, root / "output", "1.2.3")

    def test_final_bundle_rejects_invalid_npm_entry(self):
        for key, value in (
            ("tarball", "dir/package.tgz"), ("tarball", "../package.tgz"),
            ("tarball", "dir\\package.tgz"), ("tarball", ""),
            ("sha256", "F" * 64), ("sha256", "0" * 63),
            ("sha256", "0" * 64), ("size", 1),
            ("size", True), ("size", 0), ("size", -1), ("size", 2.5),
            ("name", ""), ("name", "  "), ("version", ""),
        ):
            with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                files = self.final_inputs(root)
                path = root / "npm-index.json"
                data = json.loads(path.read_text())
                data["packages"][0][key] = value
                path.write_text(json.dumps(data))
                with self.assertRaises(release.ReleaseError):
                    release.assemble_final(files, root / "output", "1.2.3")

    def test_final_bundle_rejects_duplicate_or_unsorted_npm_packages(self):
        for name, tarball in (("@scope/example", "second.tgz"), ("@scope/another", "second.tgz"), ("@scope/zebra", "scope-example-2.3.4.tgz")):
            with self.subTest(name=name, tarball=tarball), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                files = self.final_inputs(root)
                path = root / "npm-index.json"
                data = json.loads(path.read_text())
                data["packages"].append({**data["packages"][0], "name": name, "tarball": tarball})
                data["package_count"] = 2
                path.write_text(json.dumps(data))
                with self.assertRaisesRegex(release.ReleaseError, "unique names|duplicate npm"):
                    release.assemble_final(files, root / "output", "1.2.3")

    def test_npm_slice_is_a_producer_and_rejects_input(self):
        self.assertIn("npm-packages", release.SLICES)
        self.assertIn("npm-packages", release.PRODUCERS)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(release.ReleaseError, "does not accept"):
                release.run_slice(root, "bazel", "npm-packages", "1.2.3", root, root / "output")

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
            files = self.final_inputs(root)
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
                    files,
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
