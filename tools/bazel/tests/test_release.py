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
import zipfile

from tools.bazel import release_bundle

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("h2_release", ROOT / "scripts/bazel/bazel-release.py")
assert SPEC and SPEC.loader
release = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = release
SPEC.loader.exec_module(release)
BATCH = "20260920-120000"
ARCHIVE = f"firmware-release-v{BATCH}.zip"


def catalog_entry(board="devkit", platform="esp", target="esp32s3", version="0.1.0"):
    entry = release_bundle.LOADER_ROOT + board
    return dict(entry=entry, label="//" + entry + ":package", board=board,
                image="loader", role="h2loader", platform=platform, target=target,
                version=version, release_name="loader-" + board)


def query_xml(labels, tag="firmware-release"):
    return '<query>' + ''.join(
        f'<rule name="{label}"><list name="tags"><string value="{tag}"/></list></rule>'
        for label in labels) + '</query>'


class ReleaseTest(unittest.TestCase):
    def firmware_inputs(self, root):
        item = catalog_entry()
        item["package_manifest"] = dict(format=1, board=item["board"], role=item["role"],
            target=item["target"], version=item["version"], image_size=8, image_sha256="0" * 64)
        item["assets"] = []
        for suffix, operation in sorted(release_bundle.expected_asset_contracts(item)):
            path = root / (item["release_name"] + suffix)
            path.write_bytes(b"firmware" + suffix.encode())
            asset = dict(name=path.name, release_suffix=suffix, operation=operation,
                         size=path.stat().st_size, sha256=release.sha256(path))
            if operation == "factory-flash":
                asset["flash_offset"] = 0
            item["assets"].append(asset)
        (root / "firmware-index.json").write_text(json.dumps(dict(
            format=1, batch=BATCH, firmware_count=1, firmware=[item])))
        self.write_checksums(root)
        return list(root.iterdir())

    def write_checksums(self, root):
        (root / "SHA256SUMS").write_text(''.join(
            f"{release.sha256(p)}  {p.name}\n" for p in sorted(root.iterdir())
            if p.name != "SHA256SUMS"))

    def final_inputs(self, root):
        with tempfile.TemporaryDirectory() as directory:
            release.package_bundle(self.firmware_inputs(Path(directory)), root, BATCH)
        tarball = root / "scope-example-2.3.4.tgz"
        tarball.write_bytes(b"npm tarball fixture")
        (root / "npm-index.json").write_text(json.dumps(dict(
            format=1, version=BATCH, package_count=1, packages=[dict(
                name="@scope/example", version="2.3.4", tarball=tarball.name,
                sha256=release.sha256(tarball), size=tarball.stat().st_size)])))
        return list(root.iterdir())

    def test_batch_validates_format_calendar_and_zip_range(self):
        for value in (BATCH, "20240229-235959", "19800101-000000", "21071231-235959"):
            release.validate_batch(value)
        for value in ("", "1.2.3", "v" + BATCH, "2026092-120000", "20260920-120000\n",
                      "20260229-120000", "20261301-000000", "20260920-240000", "19791231-235959", "21080101-000000"):
            with self.subTest(value=value), self.assertRaises(release.ReleaseError):
                release.validate_batch(value)

    def test_catalog_rejects_diagnostics_even_when_tagged(self):
        for entry, label in (
            ("projects/e2e/targets/h2loader_tar_zlib/pal/devkit", None),
            ("projects/example/targets/h2loader_tar_zlib/button/devkit", None),
            ("projects/h2loader/targets/h2loader_tar_zlib/e2e-app/devkit", None),
            (release_bundle.LOADER_ROOT + "devkit", ":package_usb"),
        ):
            with self.subTest(entry=entry, label=label), tempfile.TemporaryDirectory() as directory:
                item = {**catalog_entry(), "entry": entry, "label": "//" + entry + (label or ":package")}
                path = Path(directory) / "firmware-catalog.json"
                path.write_text(json.dumps([item]))
                with self.assertRaisesRegex(release.ReleaseError, "diagnostic"):
                    release.load_catalog([path])

    def test_catalog_rejects_bad_version_release_name_and_platform(self):
        for key, value in (("release_name", "wrong"), ("version", "1.02.3"),
                           ("version", "20260920-120000"), ("platform", "bk3633")):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "firmware-catalog.json"
                path.write_text(json.dumps([{**catalog_entry(), key: value}]))
                with self.assertRaises(release.ReleaseError):
                    release.load_catalog([path])

    def test_catalog_queries_all_configs_with_opt_in_and_independent_versions(self):
        entries = [catalog_entry(), catalog_entry("p4", target="esp32p4", version="2.0.0"),
                   catalog_entry("bk", "bk7258", "bk7258"), catalog_entry("jl", "jieli", "wl82")]
        results = [subprocess.CompletedProcess([], 0, query_xml([i["label"] for i in entries]), "")]
        results += [subprocess.CompletedProcess([], 0, json.dumps(item), "") for item in entries]
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(release, "command", side_effect=results) as command:
            release.build_catalog(ROOT, "bazel", Path(directory))
            self.assertEqual(command.call_count, 5)
            self.assertEqual(command.call_args_list[0].args[1][2:4], [release.RELEASE_QUERY, "--output=xml"])
            self.assertIn('attr("tags", "firmware-release",', release.RELEASE_QUERY)
            for config, call in zip(release.CATALOG_CONFIGS, command.call_args_list[1:]):
                self.assertIn("--config=" + config, call.args[1])
                self.assertFalse(any("firmware_version=" in arg for arg in call.args[1]))
            data = json.loads((Path(directory) / "firmware-catalog.json").read_text())
            self.assertEqual({i["version"] for i in data}, {"0.1.0", "2.0.0"})

    def test_catalog_rejects_partial_tag_matches_and_missing_coverage(self):
        for tag in ("not-firmware-release", "firmware-release-extra"):
            with self.subTest(tag=tag), mock.patch.object(release, "command", return_value=
                    subprocess.CompletedProcess([], 0, query_xml([catalog_entry()["label"]], tag), "")):
                with self.assertRaisesRegex(release.ReleaseError, "exactly one"):
                    release.build_catalog(ROOT, "bazel", Path("unused"))
        results = [subprocess.CompletedProcess([], 0, query_xml([catalog_entry()["label"], catalog_entry("missing")["label"]]), "")]
        results += [subprocess.CompletedProcess([], 0, json.dumps(catalog_entry()) if i == 0 else "", "") for i in range(4)]
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(release, "command", side_effect=results):
            with self.assertRaisesRegex(release.ReleaseError, "coverage"):
                release.build_catalog(ROOT, "bazel", Path(directory))

    def test_catalog_rejects_starlark_errors_with_zero_exit(self):
        results = [subprocess.CompletedProcess([], 0, query_xml([catalog_entry()["label"]]), ""),
                   subprocess.CompletedProcess([], 0, "", "ERROR: Starlark evaluation error")]
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(release, "command", side_effect=results):
            with self.assertRaisesRegex(release.ReleaseError, "reported an error"):
                release.build_catalog(ROOT, "bazel", Path(directory))

    def test_s3_release_keeps_wifi_credentials_out_of_argv(self):
        credentials = '{"ssid":"fixture","password":"fixture"}'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "firmware-catalog.json"
            catalog.write_text(json.dumps([catalog_entry()]))
            asset = root / "asset"
            asset.write_bytes(b"firmware")
            output = root / "output"
            output.mkdir()
            with mock.patch.dict(os.environ, {"H2LOADER_WIFI_CREDENTIALS": credentials}), mock.patch.object(release, "command", side_effect=[
                    subprocess.CompletedProcess([], 0, "", ""), subprocess.CompletedProcess([], 0, str(asset) + "\n", "")]) as command:
                release.build_firmware(root, "bazel", "esp32s3", [catalog], output)
            args = command.call_args_list[0].args[1]
            self.assertNotIn(credentials, " ".join(args))
            self.assertTrue(args[1].startswith("--bazelrc="))
            self.assertEqual(args[2:4], ["build", "--noannounce_rc"])
            self.assertFalse(Path(args[1].partition("=")[2]).exists())
            self.assertFalse(any("firmware_version=" in arg for call in command.call_args_list for arg in call.args[1]))

    def test_zip_determinism_layout_and_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = root / "input"
            inputs.mkdir()
            files = self.firmware_inputs(inputs)
            first, second = root / "first", root / "second"
            first.mkdir(); second.mkdir()
            release.package_bundle(files, first, BATCH)
            for path in files:
                os.utime(path, (1234567890, 1234567890))
                path.chmod(0o755)
            release.package_bundle(list(reversed(files)), second, BATCH)
            self.assertEqual((first / ARCHIVE).read_bytes(), (second / ARCHIVE).read_bytes())
            with zipfile.ZipFile(first / ARCHIVE) as archive:
                self.assertEqual(archive.namelist(), [ARCHIVE[:-4] + "/" + p.name for p in sorted(files)])
                for info in archive.infolist():
                    self.assertEqual(info.date_time, (2026, 9, 20, 12, 0, 0))
                    self.assertEqual(info.create_system, 3)
                    self.assertEqual(info.external_attr, 0o100644 << 16)
                    self.assertEqual(info.compress_type, zipfile.ZIP_DEFLATED)
            release.validate_archive(first / ARCHIVE, BATCH)

    def test_package_rejects_checksum_coverage_tamper_and_extra_inputs(self):
        for mutation in ("missing-checksum", "tamper", "extra", "batch", "release-name", "missing-asset"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                files = self.firmware_inputs(root)
                if mutation == "missing-checksum":
                    (root / "SHA256SUMS").write_text((root / "SHA256SUMS").read_text().splitlines()[0] + "\n")
                elif mutation == "tamper":
                    (root / "loader-devkit.update.tar.zlib").write_bytes(b"tampered")
                elif mutation == "extra":
                    extra = root / "debug.log"; extra.write_text("private"); files.append(extra)
                else:
                    path = root / "firmware-index.json"
                    index = json.loads(path.read_text())
                    if mutation == "batch": index["batch"] = "20260920-120001"
                    elif mutation == "release-name": index["firmware"][0]["release_name"] = "wrong"
                    else: index["firmware"][0]["assets"].pop()
                    path.write_text(json.dumps(index)); self.write_checksums(root)
                with self.assertRaises(release.ReleaseError):
                    release.package_bundle(files, root / "output", BATCH)

    def test_final_bundle_has_four_assets_and_three_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); inputs = root / "input"; inputs.mkdir()
            output = root / "output"; output.mkdir()
            release.assemble_final(self.final_inputs(inputs), output, BATCH)
            expected = {ARCHIVE, "scope-example-2.3.4.tgz", "npm-index.json"}
            self.assertEqual({p.name for p in output.iterdir()}, expected | {"SHA256SUMS"})
            release.validate_checksums(output / "SHA256SUMS", {p.name: p for p in output.iterdir()}, expected)

    def test_final_slice_merges_separate_artifacts_and_multiple_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = root / "input"
            inputs.mkdir()
            for path in self.final_inputs(inputs):
                artifact = inputs / ("firmware" if path.suffix == ".zip" else "npm")
                artifact.mkdir(exist_ok=True)
                path.rename(artifact / path.name)
            npm = inputs / "npm"
            second = npm / "zebra-4.5.6.tgz"
            second.write_bytes(b"second package")
            index_path = npm / "npm-index.json"
            index = json.loads(index_path.read_text())
            index["packages"].append(dict(name="zebra", version="4.5.6", tarball=second.name,
                sha256=release.sha256(second), size=second.stat().st_size))
            index["package_count"] = 2
            index_path.write_text(json.dumps(index))
            output = root / "output"
            release.run_slice(root, "unused-bazel", "release-bundle", BATCH, inputs, output)
            expected = {ARCHIVE, "npm-index.json", "scope-example-2.3.4.tgz", second.name}
            self.assertEqual({p.name for p in output.iterdir()}, expected | {"SHA256SUMS"})
            release.validate_checksums(output / "SHA256SUMS", {p.name: p for p in output.iterdir()}, expected)
            # Duplicated artifact names fail before clearing the valid output.
            (npm / ARCHIVE).write_bytes(b"unexpected duplicate")
            with self.assertRaisesRegex(release.ReleaseError, "duplicate"):
                release.run_slice(root, "unused-bazel", "release-bundle", BATCH, inputs, output)
            self.assertEqual({p.name for p in output.iterdir()}, expected | {"SHA256SUMS"})

    def test_final_revalidates_zip_interior(self):
        for mutation in ("tamper", "checksum-coverage", "extra", "duplicate", "traversal", "size", "batch"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                files = self.final_inputs(root)
                path = root / ARCHIVE
                with zipfile.ZipFile(path) as archive:
                    contents = [(i, archive.read(i)) for i in archive.infolist()]
                with zipfile.ZipFile(path, "w") as archive:
                    for info, data in contents:
                        if mutation == "tamper" and info.filename.endswith(".update.tar.zlib"): data = b"tampered"
                        if mutation == "checksum-coverage" and info.filename.endswith("SHA256SUMS"): data = data.splitlines()[0] + b"\n"
                        if mutation in {"size", "batch"} and info.filename.endswith("firmware-index.json"):
                            index = json.loads(data)
                            if mutation == "batch": index["batch"] = "20260920-120001"
                            else: index["firmware"][0]["assets"][0]["size"] += 1
                            data = json.dumps(index).encode()
                        archive.writestr(info, data)
                    if mutation in {"extra", "duplicate", "traversal"}:
                        info = zipfile.ZipInfo(ARCHIVE[:-4] + "/" + {"extra": "extra", "duplicate": "SHA256SUMS", "traversal": "../escape"}[mutation])
                        info.create_system = 3; info.external_attr = 0o100644 << 16; info.compress_type = zipfile.ZIP_DEFLATED
                        archive.writestr(info, b"extra")
                with self.assertRaises(release.ReleaseError):
                    release.assemble_final(files, root / "output", BATCH)

    def test_retired_desktop_slice_is_not_registered(self):
        self.assertNotIn("desktop-" + "macos-arm64", release.SLICES)

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

    def test_final_bundle_rejects_missing_npm_index(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.final_inputs(root)
            with self.assertRaisesRegex(release.ReleaseError, "incomplete.*npm-index.json"):
                release.assemble_final([path for path in files if path.name != "npm-index.json"], root / "output", BATCH)

    def test_final_bundle_rejects_corrupt_npm_tarball(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.final_inputs(root)
            tarball = root / "scope-example-2.3.4.tgz"
            # Preserve size so this exercises the digest check specifically.
            tarball.write_bytes(b"x" * tarball.stat().st_size)
            with self.assertRaisesRegex(release.ReleaseError, "tarball integrity mismatch"):
                release.assemble_final(files, root / "output", BATCH)

    def test_final_bundle_rejects_missing_npm_tarball(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.final_inputs(root)
            with self.assertRaisesRegex(release.ReleaseError, "inputs differ: missing=.*scope-example"):
                release.assemble_final([path for path in files if path.suffix != ".tgz"], root / "output", BATCH)

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
                    release.assemble_final(files, root / "output", BATCH)

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
                    release.assemble_final(files, root / "output", BATCH)

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
                    release.assemble_final(files, root / "output", BATCH)

    def test_npm_slice_is_a_producer_and_rejects_input(self):
        self.assertIn("npm-packages", release.SLICES)
        self.assertIn("npm-packages", release.PRODUCERS)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(release.ReleaseError, "does not accept"):
                release.run_slice(root, "bazel", "npm-packages", BATCH, root, root / "output")

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
                    BATCH,
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
                    BATCH,
                    root,
                    root / "output",
                )


if __name__ == "__main__":
    unittest.main()
