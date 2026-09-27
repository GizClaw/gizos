from __future__ import annotations

import gzip
import hashlib
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest

from tools.bazel.npm_release import assemble, pack


class NpmReleaseTest(unittest.TestCase):
    def package(self, root: Path, name: str = "@scope/example", version: str = "2.3.4") -> tuple[Path, Path]:
        package = root / "package"
        package.mkdir(parents=True)
        manifest = root / "package.json"
        manifest.write_text(json.dumps({
            "name": name,
            "version": version,
            "files": ["lib/index.js", "README.md"],
        }), encoding="utf-8")
        (package / "package.json").write_bytes(manifest.read_bytes())
        (package / "lib").mkdir()
        (package / "lib/index.js").write_bytes(b"export default 42;\n")
        (package / "README.md").write_bytes(b"Example\n")
        return package, manifest

    def test_reproducible_pack_ignores_source_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package, manifest = self.package(root)
            first = pack(package, manifest, root / "first")
            for path in package.rglob("*"):
                os.utime(path, (1234567890, 1234567890))
                if path.is_file():
                    path.chmod(0o755)
            second = pack(package, manifest, root / "second")
            self.assertEqual(first.name, "scope-example-2.3.4.tgz")
            self.assertEqual(first.read_bytes(), second.read_bytes())
            header = first.read_bytes()[:10]
            self.assertEqual(header[:4], b"\x1f\x8b\x08\x00")
            self.assertEqual(header[4:8], b"\x00" * 4)
            self.assertEqual(gzip.decompress(first.read_bytes())[257:263], b"ustar\x00")
            with tarfile.open(first) as archive:
                members = archive.getmembers()
                self.assertEqual([item.name for item in members], [
                    "package", "package/README.md", "package/lib", "package/lib/index.js", "package/package.json",
                ])
                for item in members:
                    self.assertEqual((item.mtime, item.uid, item.gid, item.uname, item.gname), (0, 0, 0, "", ""))
                    self.assertEqual(item.mode, 0o755 if item.isdir() else 0o644)
                    self.assertEqual(item.pax_headers, {})
                self.assertEqual(archive.extractfile("package/lib/index.js").read(), b"export default 42;\n")

    def test_rejects_missing_or_extra_file(self) -> None:
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                package, manifest = self.package(root)
                if missing:
                    (package / "README.md").unlink()
                else:
                    (package / "private.key").write_bytes(b"unexpected")
                with self.assertRaisesRegex(ValueError, "files differ"):
                    pack(package, manifest, root / "output")

    def test_rejects_missing_or_invalid_identity(self) -> None:
        for key in ("name", "version"):
            for value in (None, "", 42, "../../escape"):
                with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    package, manifest = self.package(root)
                    data = json.loads(manifest.read_text())
                    if value is None:
                        del data[key]
                    else:
                        data[key] = value
                    manifest.write_text(json.dumps(data))
                    with self.assertRaisesRegex(ValueError, f"invalid {key}"):
                        pack(package, manifest, root / "output")

    def test_rejects_unsafe_file_list(self) -> None:
        for files in (None, ["../secret"], ["/secret"], ["lib\\index.js"], ["lib/*.js"], ["README.md", "README.md"]):
            with self.subTest(files=files), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                package, manifest = self.package(root)
                data = json.loads(manifest.read_text())
                data["files"] = files
                manifest.write_text(json.dumps(data))
                with self.assertRaisesRegex(ValueError, "files"):
                    pack(package, manifest, root / "output")

    def test_manifest_mismatch_and_bazel_sandbox_file_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package, manifest = self.package(root)
            (package / "package.json").write_text("{}")
            with self.assertRaisesRegex(ValueError, "differs from its manifest"):
                pack(package, manifest, root / "output")
            (package / "package.json").write_bytes(manifest.read_bytes())
            (package / "README.md").unlink()
            (package / "README.md").symlink_to(manifest)
            tarball = pack(package, manifest, root / "output")
            with tarfile.open(tarball) as archive:
                self.assertTrue(archive.getmember("package/README.md").isfile())
                self.assertEqual(archive.extractfile("package/README.md").read(), manifest.read_bytes())
            (package / "linked-directory").symlink_to(package, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "directory symlink"):
                pack(package, manifest, root / "output")

    def test_assembles_multiple_packages_sorted_with_independent_versions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            packages = []
            for index, (name, version) in enumerate((("zebra", "1.0.0-rc.1"), ("@scope/alpha", "2.3.4"))):
                package, manifest = self.package(root / str(index), name, version)
                tarball = pack(package, manifest, root / f"tarball-{index}")
                packages.append((tarball.parent, manifest))
            output = root / "output"
            assemble(packages, output, "20260920-120000")
            text = (output / "npm-index.json").read_text()
            index = json.loads(text)
            self.assertEqual(text, json.dumps(index, indent=2, sort_keys=True) + "\n")
            self.assertEqual(set(index), {"format", "version", "package_count", "packages"})
            self.assertEqual(index["format"], 1)
            self.assertEqual(index["version"], "20260920-120000")
            self.assertEqual(index["package_count"], 2)
            self.assertEqual([item["name"] for item in index["packages"]], ["@scope/alpha", "zebra"])
            self.assertEqual([item["version"] for item in index["packages"]], ["2.3.4", "1.0.0-rc.1"])
            self.assertEqual({path.name for path in output.iterdir()}, {"npm-index.json", "scope-alpha-2.3.4.tgz", "zebra-1.0.0-rc.1.tgz"})
            for item in index["packages"]:
                self.assertEqual(set(item), {"name", "version", "tarball", "sha256", "size"})
                data = (output / item["tarball"]).read_bytes()
                self.assertEqual(item["sha256"], hashlib.sha256(data).hexdigest())
                self.assertEqual(item["size"], len(data))

    def test_batch_changes_only_index_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package, manifest = self.package(root)
            tarball = pack(package, manifest, root / "tarball")
            for batch in ("20260920-120000", "20260921-120000"):
                output = root / batch
                assemble([(tarball.parent, manifest)], output, batch)
                index = json.loads((output / "npm-index.json").read_text())
                self.assertEqual(index["version"], batch)
                self.assertEqual(index["packages"][0]["version"], "2.3.4")
                self.assertEqual((output / tarball.name).read_bytes(), tarball.read_bytes())

    def test_bundle_rejects_empty_duplicate_and_extra_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package, manifest = self.package(root)
            tarball = pack(package, manifest, root / "tarball")
            inputs = [(tarball.parent, manifest)]
            with self.assertRaisesRegex(ValueError, "at least one"):
                assemble([], root / "output", "1.2.3")
            with self.assertRaisesRegex(ValueError, "duplicate"):
                assemble(inputs * 2, root / "output", "1.2.3")
            (tarball.parent / "SHA256SUMS").write_text("unexpected")
            with self.assertRaisesRegex(ValueError, "exactly one tarball"):
                assemble(inputs, root / "output", "1.2.3")


if __name__ == "__main__":
    unittest.main()
