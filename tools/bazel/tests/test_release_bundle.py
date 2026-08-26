from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools.bazel.release_bundle import assemble


class ReleaseBundleTest(unittest.TestCase):
    def test_assembles_exact_catalog_with_verified_asset(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = root / "board-app-chip.update.tar.zlib"
            asset.write_bytes(b"package")
            identity = {
                "entry": "projects/example/targets/h2loader_tar_zlib/app/board",
                "platform": "esp",
                "project": "example",
                "app": "app",
                "board": "board",
                "image": "app",
                "role": "app",
                "target": "chip",
            }
            catalog = root / "firmware-catalog.json"
            catalog.write_text(json.dumps([{**identity, "label": "//entry:firmware", "version": "2.0.0"}]))
            metadata = root / "board-app-chip.firmware.json"
            metadata.write_text(json.dumps({
                **identity,
                "version": "2.0.0",
                "package_manifest": {
                    "format": 1,
                    "role": "app",
                    "board": "board",
                    "target": "chip",
                    "version": "2.0.0",
                    "image_size": 7,
                    "image_sha256": "0" * 64,
                },
                "assets": [{
                    "name": asset.name,
                    "operation": "managed-install",
                    "sha256": hashlib.sha256(asset.read_bytes()).hexdigest(),
                    "size": asset.stat().st_size,
                }],
                "native_artifacts": [],
            }))
            output = root / "output"
            output.mkdir()

            assemble([catalog, metadata, asset], output, "1.2.3")

            index = json.loads((output / "firmware-index.json").read_text())
            self.assertEqual(index["firmware_count"], 1)
            self.assertEqual(index["version"], "1.2.3")
            self.assertEqual(index["firmware"][0]["version"], "2.0.0")
            self.assertEqual(index["firmware"][0]["entry"], identity["entry"])
            self.assertEqual(
                index["firmware"][0]["package_manifest"]["image_sha256"],
                "0" * 64,
            )
            self.assertEqual((output / asset.name).read_bytes(), b"package")
            self.assertIn(asset.name, (output / "SHA256SUMS").read_text())

    def test_assembles_multiple_firmware_versions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog_entries = []
            inputs = []
            for board, version in (("board-a", "1.2.3"), ("board-b", "4.5.6-rc.1")):
                identity = {
                    "entry": f"projects/example/{board}",
                    "platform": "esp",
                    "project": "example",
                    "app": "app",
                    "board": board,
                    "image": "app",
                    "role": "app",
                    "target": "chip",
                }
                catalog_entries.append({**identity, "version": version})
                asset = root / f"{board}-app-chip.update.tar.zlib"
                asset.write_bytes(board.encode("ascii"))
                metadata = root / f"{board}-app-chip.firmware.json"
                metadata.write_text(json.dumps({
                    **identity,
                    "version": version,
                    "package_manifest": {
                        "format": 1,
                        "role": "app",
                        "board": board,
                        "target": "chip",
                        "version": version,
                        "image_size": len(board),
                        "image_sha256": "0" * 64,
                    },
                    "assets": [{
                        "name": asset.name,
                        "sha256": hashlib.sha256(asset.read_bytes()).hexdigest(),
                        "size": asset.stat().st_size,
                    }],
                }))
                inputs.extend([metadata, asset])
            catalog = root / "firmware-catalog.json"
            catalog.write_text(json.dumps(catalog_entries))

            output = root / "output"
            assemble([catalog, *inputs], output, "2026.8.26")

            index = json.loads((output / "firmware-index.json").read_text())
            self.assertEqual(index["version"], "2026.8.26")
            self.assertEqual(
                {item["version"] for item in index["firmware"]},
                {"1.2.3", "4.5.6-rc.1"},
            )

    def test_preserves_desktop_release_assets(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = root / "board-app-chip.update.tar.zlib"
            asset.write_bytes(b"package")
            desktop = root / "h2-desktop-linux.zip"
            desktop.write_bytes(b"desktop")
            desktop_checksum = root / "h2-desktop-linux.zip.sha256"
            desktop_checksum.write_text("checksum\n", encoding="ascii")
            identity = {
                "entry": "projects/example/targets/h2loader_tar_zlib/app/board",
                "platform": "esp",
                "project": "example",
                "app": "app",
                "board": "board",
                "image": "app",
                "role": "app",
                "target": "chip",
            }
            catalog = root / "firmware-catalog.json"
            catalog.write_text(json.dumps([{**identity, "version": "1.2.3"}]))
            metadata = root / "board-app-chip.firmware.json"
            metadata.write_text(json.dumps({
                **identity,
                "version": "1.2.3",
                "package_manifest": {
                    "format": 1,
                    "role": "app",
                    "board": "board",
                    "target": "chip",
                    "version": "1.2.3",
                    "image_size": 7,
                    "image_sha256": "0" * 64,
                },
                "assets": [{
                    "name": asset.name,
                    "sha256": hashlib.sha256(asset.read_bytes()).hexdigest(),
                    "size": asset.stat().st_size,
                }],
            }))
            output = root / "output"

            assemble(
                [catalog, metadata, asset, desktop, desktop_checksum],
                output,
                "1.2.3",
            )

            self.assertEqual((output / desktop.name).read_bytes(), b"desktop")
            self.assertEqual(
                (output / desktop_checksum.name).read_text(encoding="ascii"),
                "checksum\n",
            )
            checksums = (output / "SHA256SUMS").read_text(encoding="ascii")
            self.assertIn(desktop.name, checksums)
            self.assertIn(desktop_checksum.name, checksums)

    def test_rejects_invalid_catalog_version(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "firmware-catalog.json"
            catalog.write_text(json.dumps([{
                "entry": "entry",
                "platform": "esp",
                "project": "example",
                "app": "app",
                "board": "board",
                "image": "app",
                "role": "app",
                "target": "chip",
                "version": "other",
            }]))
            with self.assertRaisesRegex(ValueError, "invalid firmware version"):
                assemble([catalog], root / "output", "1.2.3")

    def test_rejects_catalog_metadata_version_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = root / "board-app-chip.update.tar.zlib"
            asset.write_bytes(b"package")
            identity = {
                "entry": "projects/example/targets/h2loader_tar_zlib/app/board",
                "platform": "esp",
                "project": "example",
                "app": "app",
                "board": "board",
                "image": "app",
                "role": "app",
                "target": "chip",
            }
            catalog = root / "firmware-catalog.json"
            catalog.write_text(json.dumps([{**identity, "version": "1.2.3"}]))
            metadata = root / "board-app-chip.firmware.json"
            metadata.write_text(json.dumps({
                **identity,
                "version": "1.2.4",
                "package_manifest": {
                    "format": 1,
                    "role": "app",
                    "board": "board",
                    "target": "chip",
                    "version": "1.2.4",
                    "image_size": 7,
                    "image_sha256": "0" * 64,
                },
                "assets": [{
                    "name": asset.name,
                    "sha256": hashlib.sha256(asset.read_bytes()).hexdigest(),
                    "size": asset.stat().st_size,
                }],
            }))
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                assemble([catalog, metadata, asset], root / "output", "release-1")

    def test_rejects_missing_catalog_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "firmware-catalog.json"
            catalog.write_text(json.dumps([{
                "entry": "missing",
                "platform": "esp",
                "project": "example",
                "app": "app",
                "board": "board",
                "image": "app",
                "role": "app",
                "target": "chip",
                "version": "1.2.3",
            }]))
            with self.assertRaisesRegex(ValueError, "coverage mismatch"):
                assemble([catalog], root / "output", "1.2.3")

    def test_rejects_catalog_metadata_project_app_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = root / "example-app-board.update.tar.zlib"
            asset.write_bytes(b"package")
            identity = {
                "entry": "projects/example/targets/h2loader_tar_zlib/app/board",
                "platform": "esp",
                "project": "example",
                "app": "app",
                "board": "board",
                "image": "app",
                "role": "app",
                "target": "chip",
            }
            catalog = root / "firmware-catalog.json"
            catalog.write_text(json.dumps([{**identity, "version": "1.2.3"}]))
            metadata = root / "example-app-board.firmware.json"
            for field in ("project", "app"):
                with self.subTest(field=field):
                    metadata.write_text(json.dumps({
                        **identity,
                        field: "other",
                        "version": "1.2.3",
                        "package_manifest": {
                            "format": 1,
                            "role": "app",
                            "board": "board",
                            "target": "chip",
                            "version": "1.2.3",
                            "image_size": 7,
                            "image_sha256": "0" * 64,
                        },
                        "assets": [{
                            "name": asset.name,
                            "sha256": hashlib.sha256(asset.read_bytes()).hexdigest(),
                            "size": asset.stat().st_size,
                        }],
                    }))

                    with self.assertRaisesRegex(ValueError, "identity mismatch"):
                        assemble(
                            [catalog, metadata, asset],
                            root / f"output-{field}",
                            "release-1",
                        )


if __name__ == "__main__":
    unittest.main()
