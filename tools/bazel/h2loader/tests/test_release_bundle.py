from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools.bazel.h2loader.release_bundle import assemble, expected_asset_contracts, LOADER_ENTRIES, validate_catalog, validate_index

BATCH = "20260920-120000"


class ReleaseBundleTest(unittest.TestCase):
    def inputs(self, root, entries=(("devkit", "esp", "esp32s3", "0.1.0"),), package_format=1):
        catalog = []
        files = []
        for board, platform, target, version in entries:
            entry = LOADER_ENTRIES[board]
            if package_format == 2:
                entry = entry.replace("/h2_loader_tar_zlib/", "/h2_loader_zlib_tar/")
            package_target = "package_usb" if board == "devkit" else "package"
            identity = dict(entry=entry, label="//" + entry + ":" + package_target, platform=platform,
                board=board, image="loader", role="h2loader", target=target,
                version=version, release_name="loader-" + board)
            catalog.append(identity)
            if package_format != 1:
                identity["package_format"] = package_format
            metadata = {**identity, "package_manifest": dict(format=package_format, role="h2loader",
                board=board, target=target, version=version, image_size=7, image_sha256="0" * 64), "assets": []}
            for suffix, operation in sorted(expected_asset_contracts(identity)):
                asset = root / f"{board}-loader-{target}{suffix}"
                asset.write_bytes((board + suffix).encode())
                data = dict(name=asset.name, release_suffix=suffix, operation=operation,
                    sha256=hashlib.sha256(asset.read_bytes()).hexdigest(), size=asset.stat().st_size)
                if operation == "factory-flash": data["flash_offset"] = 0
                metadata["assets"].append(data)
                files.append(asset)
            path = root / f"{board}-loader-{target}.firmware.json"
            path.write_text(json.dumps(metadata)); files.append(path)
        path = root / "firmware-catalog.json"
        path.write_text(json.dumps(catalog)); files.append(path)
        return files

    def legacy_format_two_index(self, root):
        files = self.inputs(root, (("devkit", "esp", "esp32s3", "1.2.3"),
            ("bk7258_v3_202405", "bk7258", "bk7258", "4.5.6"),
            ("jieli_ac791n_devkit", "jieli", "wl82", "0.2.0"),
            ("waveshare_esp32p4_wifi6_touch_lcd_4_3", "esp", "esp32p4", "2.0.0")), package_format=2)
        assemble(files, root / "output", BATCH)
        index = json.loads((root / "output/firmware-index.json").read_text())
        for item in index["firmware"]:
            item["entry"] = LOADER_ENTRIES[item["board"]]
            package_target = "package_usb_v2" if item["board"] == "devkit" else "package_v2"
            item["label"] = "//" + item["entry"] + ":" + package_target
        return index

    def test_index_accepts_legacy_format_two_entries_without_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            index = self.legacy_format_two_index(Path(directory))
            original = copy.deepcopy(index)
            assets = validate_index(index, BATCH)
            self.assertEqual(assets, {asset["name"]: asset for item in index["firmware"] for asset in item["assets"]})
            self.assertEqual(index, original)
            for item in index["firmware"]:
                with self.subTest(board=item["board"]):
                    package_target = "package_usb" if item["board"] == "devkit" else "package"
                    catalog_item = {**item, "label": "//" + item["entry"] + ":" + package_target}
                    with self.assertRaisesRegex(ValueError, "diagnostic"):
                        validate_catalog([catalog_item])

    def test_legacy_index_rejects_invalid_identities(self):
        with tempfile.TemporaryDirectory() as directory:
            original = self.legacy_format_two_index(Path(directory))
            for key, value in (("entry", LOADER_ENTRIES["amoled"]),
                               ("entry", LOADER_ENTRIES["devkit"] + "/diagnostic"),
                               ("entry", "projects/testing/diagnostic"),
                               ("board", "unknown"), ("board", []),
                               ("image", "app"), ("role", "app"),
                               ("platform", "unsupported"), ("target", "unsupported"),
                               ("version", "1.02.3"), ("release_name", "wrong"),
                               ("package_format", True)):
                with self.subTest(key=key, value=value):
                    index = copy.deepcopy(original)
                    item = next(item for item in index["firmware"] if item["board"] == "devkit")
                    item[key] = value
                    if key in item["package_manifest"]:
                        item["package_manifest"][key] = value
                    with self.assertRaises(ValueError):
                        validate_index(index, BATCH)
            for current_entry in (False, True):
                with self.subTest(current_entry=current_entry):
                    index = copy.deepcopy(original)
                    duplicate = copy.deepcopy(index["firmware"][0])
                    if current_entry:
                        duplicate["entry"] = duplicate["entry"].replace("/h2_loader_tar_zlib/", "/h2_loader_zlib_tar/")
                    index["firmware"].append(duplicate)
                    index["firmware_count"] += 1
                    with self.assertRaisesRegex(ValueError, "duplicate"):
                        validate_index(index, BATCH)

    def test_assembles_exact_catalog_and_renames_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.inputs(root)
            output = root / "output"
            assemble(files, output, BATCH)
            index = json.loads((output / "firmware-index.json").read_text())
            self.assertEqual(index["batch"], BATCH)
            self.assertNotIn("version", index)
            self.assertEqual(index["firmware_count"], 1)
            item = index["firmware"][0]
            self.assertEqual(item["version"], "0.1.0")
            self.assertEqual(item["release_name"], "loader-devkit")
            self.assertEqual(item["package_manifest"]["image_sha256"], "0" * 64)
            expected = {"loader-devkit" + suffix for suffix, _ in expected_asset_contracts(item)}
            self.assertEqual({p.name for p in output.iterdir()}, expected | {"firmware-index.json", "SHA256SUMS"})
            for asset in item["assets"]:
                self.assertEqual((output / asset["name"]).read_bytes(), ("devkit" + asset["release_suffix"]).encode())
                self.assertIn(asset["name"], (output / "SHA256SUMS").read_text())

    def test_assembles_independent_package_format(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.inputs(root, package_format=2)
            assemble(files, root / "output", BATCH)
            self.assertTrue((root / "output/loader-devkit.update.tar").is_file())
            self.assertFalse((root / "output/loader-devkit.update.tar.zlib").exists())

    def test_rejects_format_type_and_contract_mismatch(self):
        for value in (True, 2.0, 0, 3, "2"):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); files = self.inputs(root, package_format=2)
                path = root / "devkit-loader-esp32s3.firmware.json"
                metadata = json.loads(path.read_text())
                metadata["package_format"] = value
                path.write_text(json.dumps(metadata))
                with self.assertRaises(ValueError): assemble(files, root / "output", BATCH)
        for kind in ("manifest", "catalog", "suffix"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); files = self.inputs(root, package_format=2)
                path = root / "devkit-loader-esp32s3.firmware.json"
                metadata = json.loads(path.read_text())
                if kind == "manifest": metadata["package_manifest"]["format"] = 1
                elif kind == "catalog":
                    catalog = json.loads((root / "firmware-catalog.json").read_text())
                    catalog[0]["package_format"] = 1
                    (root / "firmware-catalog.json").write_text(json.dumps(catalog))
                else:
                    next(asset for asset in metadata["assets"] if asset["operation"] == "managed-install")["release_suffix"] = ".update.tar.zlib"
                path.write_text(json.dumps(metadata))
                with self.assertRaises(ValueError): assemble(files, root / "output", BATCH)

    def test_assembles_all_platforms_with_independent_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = self.inputs(root, (("devkit", "esp", "esp32s3", "1.2.3"),
                ("bk7258_v3_202405", "bk7258", "bk7258", "4.5.6-rc.1"),
                ("jieli_ac791n_devkit", "jieli", "wl82", "0.2.0")))
            assemble(files, root / "output", BATCH)
            index = json.loads((root / "output/firmware-index.json").read_text())
            self.assertEqual(index["firmware_count"], 3)
            self.assertEqual({i["version"] for i in index["firmware"]}, {"1.2.3", "4.5.6-rc.1", "0.2.0"})
            self.assertEqual(sum(len(i["assets"]) for i in index["firmware"]), 6)

    def test_rejects_invalid_catalog_and_metadata(self):
        mutations = (("catalog", "version", "other", "version"),
            ("catalog", "release_name", "wrong", "release_name"),
            ("catalog", "entry", "projects/testing/diagnostic", "diagnostic"),
            ("catalog", "label", "//entry:alternate_package", "diagnostic"),
            ("metadata", "version", "1.2.4", "identity mismatch"),
            ("metadata", "board", "other", "identity mismatch"),
            ("metadata", "assets", [], "no release assets"))
        for source, key, value, message in mutations:
            with self.subTest(source=source, key=key), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); files = self.inputs(root)
                path = root / ("firmware-catalog.json" if source == "catalog" else "devkit-loader-esp32s3.firmware.json")
                data = json.loads(path.read_text()); item = data[0] if source == "catalog" else data
                item[key] = value; path.write_text(json.dumps(data))
                with self.assertRaisesRegex(ValueError, message): assemble(files, root / "output", BATCH)

    def test_rejects_missing_duplicate_and_unexpected_inputs(self):
        for mutation in ("missing-metadata", "missing-asset", "duplicate", "extra-zip", "extra-sha256", "extra-log"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); files = self.inputs(root)
                if mutation == "missing-metadata": files = [p for p in files if not p.name.endswith(".firmware.json")]
                elif mutation == "missing-asset": files = [p for p in files if not p.name.endswith(".update.tar.zlib")]
                elif mutation == "duplicate": files.append(files[0])
                else:
                    extra = root / ("debug." + mutation.removeprefix("extra-")); extra.write_bytes(b"extra"); files.append(extra)
                with self.assertRaises(ValueError): assemble(files, root / "output", BATCH)

    def test_rejects_integrity_contract_and_manifest_mismatch(self):
        for mutation in ("tamper", "size", "digest", "suffix", "operation", "offset", "missing-contract", "manifest-version", "manifest-image"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); files = self.inputs(root)
                path = root / "devkit-loader-esp32s3.firmware.json"; data = json.loads(path.read_text())
                if mutation == "tamper": (root / data["assets"][0]["name"]).write_bytes(b"tampered")
                elif mutation == "missing-contract": data["assets"].pop()
                elif mutation == "manifest-version": data["package_manifest"]["version"] = "9.9.9"
                elif mutation == "manifest-image": data["package_manifest"]["image_size"] = 0
                else:
                    key, value = {"size": ("size", True), "digest": ("sha256", "0" * 64),
                        "suffix": ("release_suffix", "../escape"), "operation": ("operation", "other"), "offset": ("flash_offset", 1)}[mutation]
                    data["assets"][0][key] = value
                path.write_text(json.dumps(data))
                with self.assertRaises(ValueError): assemble(files, root / "output", BATCH)


if __name__ == "__main__":
    unittest.main()
