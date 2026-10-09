"""Round-trip, deterministic output and invalid-input tests for Lua apps."""

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
import zlib

MODULE = Path(__file__).resolve().parents[1] / "app_package.py"
sys.path.insert(0, str(MODULE.parent))
SPEC = importlib.util.spec_from_file_location("app_package", MODULE)
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)
BAZEL_OUTPUTS = [Path(path) for path in sys.argv[1:]]
sys.argv[1:] = []


class LuaAppPackageTest(unittest.TestCase):
    def fixture(self, root, *, data=False):
        entry = root / "abc.lua"
        entry.write_bytes(b'local kv = require("kv")\nkv.set("score", 100)\n')
        files = {"abc.lua": str(entry)}
        if data:
            resource = root / "tone.pcm"
            resource.write_bytes(b"\0\xff\x80binary\0")
            files["abc/tone.pcm"] = str(resource)
        return {"app_id": "abc", "version": "1.2.3", "entry": "abc.lua", "files": files}

    def build(self, root, specification):
        archive = root / "abc-1.2.3.lua-app.tar.zlib"
        manifest = root / "manifest.json"
        metadata = root / "metadata.json"
        package.build_package(specification, archive, manifest, metadata)
        return archive, manifest, metadata

    def open_archive(self, path):
        decoder = zlib.decompressobj()
        contents = decoder.decompress(path.read_bytes()) + decoder.flush()
        self.assertTrue(decoder.eof)
        self.assertEqual(decoder.unused_data, b"")
        return tarfile.open(fileobj=io.BytesIO(contents), mode="r:")

    def inspect(self, path):
        with self.open_archive(path) as archive:
            members = archive.getmembers()
            self.assertTrue(
                all(
                    member.isfile() and member.mode == 0o644 and member.mtime == 0
                    for member in members
                )
            )
            self.assertEqual(
                [member.name for member in members],
                sorted(member.name for member in members),
            )
            manifest = json.load(archive.extractfile("manifest.json"))
            self.assertEqual(
                set(archive.getnames()),
                {"manifest.json", *(item["path"] for item in manifest["files"])},
            )
            for item in manifest["files"]:
                content = archive.extractfile(item["path"]).read()
                self.assertEqual(len(content), item["size"])
                self.assertEqual(hashlib.sha256(content).hexdigest(), item["sha256"])
            return manifest

    def test_entry_without_data_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, manifest, metadata = self.build(root, self.fixture(root))
            value = self.inspect(archive)
            self.assertIsNone(value["data_dir"])
            self.assertEqual(value, json.loads(manifest.read_text()))
            release = json.loads(metadata.read_text())
            self.assertEqual(release["manifest"], value)
            self.assertEqual(
                release["package"]["sha256"],
                hashlib.sha256(archive.read_bytes()).hexdigest(),
            )
            self.assertEqual(release["package"]["size"], archive.stat().st_size)

    def test_binary_data_and_reproducible_builds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            specification = self.fixture(root, data=True)
            archive, manifest, metadata = self.build(root, specification)
            first = [path.read_bytes() for path in (archive, manifest, metadata)]
            specification["files"] = dict(
                reversed(list(specification["files"].items()))
            )
            for name in specification["files"].values():
                Path(name).touch()
            self.build(root, specification)
            self.assertEqual(
                first, [path.read_bytes() for path in (archive, manifest, metadata)]
            )
            self.assertEqual(self.inspect(archive)["data_dir"], "abc")

    def test_rejects_unsafe_paths_and_reserved_storage_files(self):
        for name in (
            "../evil",
            "/abc/evil",
            "xyz/image.png",
            "abc/.kv",
            "abc/.index",
            "abc/../evil",
            "abc//evil",
            "abc\\evil",
            "abc/evil\n",
        ):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                specification = self.fixture(root)
                specification["files"][name] = specification["files"]["abc.lua"]
                with self.assertRaises(ValueError):
                    self.build(root, specification)
                self.assertFalse((root / "abc-1.2.3.lua-app.tar.zlib").exists())

    def test_rejects_invalid_versions_and_identity(self):
        for field, value in (
            ("version", "v1.2.3"),
            ("version", "1.02.3"),
            ("version", "1.2.3-01"),
            ("app_id", "ABC"),
            ("app_id", "../abc"),
            ("entry", "xyz.lua"),
        ):
            with self.subTest(
                field=field, value=value
            ), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                specification = self.fixture(root)
                specification[field] = value
                with self.assertRaises(ValueError):
                    self.build(root, specification)

    def test_rejects_nonportable_file_collisions(self):
        for first, second in (
            ("abc/tone.pcm", "abc/TONE.pcm"),
            ("abc/image", "abc/image/tone.pcm"),
        ):
            with self.subTest(
                first=first, second=second
            ), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                specification = self.fixture(root)
                for name in (first, second):
                    specification["files"][name] = specification["files"]["abc.lua"]
                with self.assertRaises(ValueError):
                    self.build(root, specification)

    def test_compaction_reuses_the_public_compactor(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            specification = self.fixture(root)
            original = (
                b'-- comment\nlocal value = "literal  bytes"   -- note\nreturn value\n'
            )
            Path(specification["files"]["abc.lua"]).write_bytes(original)
            specification["compact"] = True
            archive, _, _ = self.build(root, specification)
            with self.open_archive(archive) as result:
                self.assertEqual(
                    result.extractfile("abc.lua").read(),
                    package.compact_source(original),
                )
            self.assertTrue(self.inspect(archive)["compact"])

    def test_rejects_bytecode_and_embedded_nul(self):
        for content in (b"\x1bLua\x55", b"local x = 1\0"):
            with self.subTest(
                content=content
            ), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                specification = self.fixture(root)
                Path(specification["files"]["abc.lua"]).write_bytes(content)
                with self.assertRaises(ValueError):
                    self.build(root, specification)

    @unittest.skipUnless(
        BAZEL_OUTPUTS, "Bazel artifacts are tested by the Bazel target"
    )
    def test_declared_bazel_rule_outputs(self):
        self.assertEqual(len(BAZEL_OUTPUTS), 6)
        manifests = []
        for index in (0, 3):
            archive, manifest, metadata = BAZEL_OUTPUTS[index:index + 3]
            value = self.inspect(archive)
            self.assertEqual(value, json.loads(manifest.read_text()))
            release = json.loads(metadata.read_text())
            self.assertEqual(release["manifest"], value)
            for field in ("format", "type", "app_id", "version", "entry", "data_dir", "compact"):
                self.assertEqual(release[field], value[field])
            self.assertEqual(
                release["package"],
                {
                    "name": archive.name,
                    "size": archive.stat().st_size,
                    "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                },
            )
            manifests.append(value)
        plain, with_data = manifests
        self.assertEqual(
            (plain["app_id"], plain["version"], plain["data_dir"]),
            ("abc", "1.2.3", None),
        )
        self.assertEqual(
            (with_data["app_id"], with_data["version"], with_data["data_dir"]),
            ("xyz", "2.0.0-rc.1", "xyz"),
        )


if __name__ == "__main__":
    unittest.main()
