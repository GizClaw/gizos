"""Documentation assembly/contract checks, including the actual Bazel tarball."""

import copy
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

import package as docs


BUILT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
TARBALL = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
ROOT = Path(os.environ.get("TEST_SRCDIR", "")) / os.environ.get("TEST_WORKSPACE", "") if os.environ.get("TEST_SRCDIR") else Path(__file__).resolve().parents[5]


class PackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.inputs = docs.Inputs(ROOT)
        cls.files = docs.assemble(cls.inputs)
        cls.manifest = json.loads(cls.files["manifest.json"])
        cls.index = json.loads(cls.files["index.json"])

    def test_reproducible_assembly(self):
        self.assertEqual(self.files, docs.assemble(self.inputs))
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "package"
            docs.write_package(self.files, output)
            self.assertEqual(self.files, {p.relative_to(output).as_posix(): p.read_bytes() for p in output.rglob("*") if p.is_file()})
            with self.assertRaisesRegex(ValueError, "new or empty"):
                docs.write_package(self.files, output)

    def test_inventory_and_static_exports(self):
        docs.validate(self.files)
        package = json.loads(self.files["package.json"])
        self.assertEqual(package["name"], "@gizclaw/lua-docs")
        self.assertEqual(package["exports"]["."], "./manifest.json")
        self.assertFalse({"scripts", "dependencies", "bin"} & package.keys())
        self.assertEqual(set(package["files"]) | {"package.json"}, set(self.files))
        self.assertTrue(all(p.endswith((".json", ".md", ".lua")) or p == "LICENSE" for p in self.files))

    def test_source_provenance(self):
        for name, expected in self.manifest["sources"].items():
            self.assertEqual(docs.sha256(self.inputs.read(name)), expected, name)
        pin = self.inputs.json(f"{docs.PACKAGE}/runtime-pin.json")
        self.assertEqual(self.manifest["runtime"]["revision"], pin["revision"])
        self.assertEqual(docs.validate_pin(self.inputs, pin), self.manifest["runtime"]["sourceContentId"])
        changed = copy.deepcopy(pin)
        changed["files"][next(iter(changed["files"]))] = "0" * 64
        with self.assertRaisesRegex(ValueError, "differs from pinned"):
            docs.validate_pin(self.inputs, changed)
        for change in ["add", "remove"]:
            changed = copy.deepcopy(pin)
            if change == "add":
                changed["files"]["libs/lua/src/added.c"] = "0" * 64
            else:
                del changed["files"][next(iter(changed["files"]))]
            with self.assertRaisesRegex(ValueError, "file set differs"):
                docs.validate_pin(self.inputs, changed)

    def test_real_module_inventory(self):
        modules = {m["id"]: m for m in self.index["modules"]}
        for name in ["runtime", "vmath", "geometry", "json", "kv", "link"]:
            self.assertTrue(modules[name]["requireable"])
        self.assertEqual(modules["gpio"]["profileStatus"], "unavailable")
        self.assertFalse(modules["button"]["requireable"])
        self.assertEqual(modules["button"]["profileStatus"], "component-adapted")
        self.assertEqual(modules["runtime"]["documents"], ["runtime", "host"])

    def test_search_and_signatures(self):
        symbols = {s["id"]: s for s in self.index["symbols"]}
        for name in ["runtime.sleep", "runtime.components.get", "capability.call", "kv.get", "audio.new_output", "geometry.project_points", "vmath.buffer", "display.capture_region", "vmath.buffer:get", "audio.output:write"]:
            self.assertIn(name, symbols)
        sleep = symbols["runtime.sleep"]["locations"][0]
        self.assertEqual(sleep["signature"], "runtime.sleep(ms)")
        self.assertIn("4294967295", sleep["summary"])
        # A concrete call in a fenced example must never become a signature.
        self.assertNotIn("runtime.sleep(10)", [l["signature"] for l in symbols["runtime.sleep"]["locations"]])
        self.assertEqual(symbols["vmath.buffer:get"]["kind"], "method")
        for symbol in symbols.values():
            for location in symbol["locations"]:
                text = self.files[location["path"]].decode()
                # Multiline header declarations can wrap between name and args.
                self.assertIn(location["signature"].split("(", 1)[0], text)

    def test_examples_match_canonical_fences(self):
        self.assertEqual(len(self.manifest["examples"]), 5)
        for example in self.manifest["examples"]:
            doc = next(d for d in self.manifest["documents"] if d["id"] == example["documentId"])
            source = self.inputs.read(doc["source"]).decode()
            expected = dict(docs.EXAMPLE.findall(source))[example["id"]] + "\n"
            self.assertEqual(self.files[example["path"]], expected.encode())
            self.assertIn(b"require(", self.files[example["path"]])

    def test_header_contract_not_a_copied_manual(self):
        for doc in self.manifest["documents"]:
            if doc.get("format") == "header":
                text = self.files[doc["path"]].decode()
                expected = docs.header_contract(self.inputs.read(doc["source"]).decode(), doc["title"])
                self.assertEqual(text, expected)
                self.assertNotIn("@file", text)
                self.assertNotIn("#ifndef", text)
        self.assertIn(b"See the [public Display contract](./display.md)", self.files["docs/display-guide.md"])

    def modified_files(self, path, data):
        files = dict(self.files)
        files[path] = data
        manifest = json.loads(files["manifest.json"])
        manifest["files"][path] = {"sha256": docs.sha256(data), "size": len(data)}
        files["manifest.json"] = docs.encoded(manifest)
        return files

    def test_rejects_modified_and_unlisted_content(self):
        files = dict(self.files)
        files["docs/runtime.md"] += b"corrupted"
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            docs.validate(files)
        files = dict(self.files, unexpected=b"extra")
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            docs.validate(files)

    def test_rejects_broken_links_and_anchors(self):
        for target in ["./missing.md", "./numeric.md#missing", "../../escape.md"]:
            data = self.files["docs/runtime.md"] + f"\n[bad]({target})\n".encode()
            with self.assertRaises(ValueError):
                docs.validate(self.modified_files("docs/runtime.md", data))

    def test_rejects_bad_locations(self):
        index = copy.deepcopy(self.index)
        index["symbols"][0]["locations"][0]["line"] = 1000000
        with self.assertRaisesRegex(ValueError, "invalid symbol location"):
            docs.validate(self.modified_files("index.json", docs.encoded(index)))
        index = copy.deepcopy(self.index)
        index["symbols"].append(index["symbols"][0])
        with self.assertRaisesRegex(ValueError, "invalid symbol index"):
            docs.validate(self.modified_files("index.json", docs.encoded(index)))

    def test_rejects_bad_signatures_and_examples(self):
        index = copy.deepcopy(self.index)
        index["symbols"][0]["locations"][0]["signature"] = "invented.api(value)"
        with self.assertRaisesRegex(ValueError, "signature does not match"):
            docs.validate(self.modified_files("index.json", docs.encoded(index)))
        path = self.manifest["examples"][0]["path"]
        with self.assertRaisesRegex(ValueError, "example differs"):
            docs.validate(self.modified_files(path, b"print('changed')\n"))

    def test_rejects_bad_runtime_provenance(self):
        for field, value in [("revision", "main"), ("sourceContentId", "0" * 64)]:
            files = dict(self.files)
            manifest = json.loads(files["manifest.json"])
            manifest["runtime"][field] = value
            files["manifest.json"] = docs.encoded(manifest)
            with self.assertRaisesRegex(ValueError, "invalid Runtime source provenance"):
                docs.validate(files)

    def test_rejects_paths_and_schema(self):
        for name in ["/etc/passwd", "../escape", "a/../b", "a\\b", "a//b", "./file"]:
            with self.assertRaises(ValueError):
                docs.safe_path(name)
        files = dict(self.files)
        manifest = json.loads(files["manifest.json"])
        manifest["schemaVersion"] = 2
        files["manifest.json"] = docs.encoded(manifest)
        with self.assertRaisesRegex(ValueError, "unsupported"):
            docs.validate(files)

    @unittest.skipUnless(BUILT and TARBALL, "Bazel supplies the assembled tree and release tarball")
    def test_actual_bazel_tree_and_tarball(self):
        actual = {p.relative_to(BUILT).as_posix(): p.read_bytes() for p in BUILT.rglob("*") if p.is_file()}
        self.assertEqual(actual, self.files)
        tarballs = list(TARBALL.glob("*.tgz"))
        self.assertEqual(len(tarballs), 1)
        with tarfile.open(tarballs[0], "r:gz") as archive:
            entries = archive.getmembers()
            self.assertTrue(all(m.isfile() or m.isdir() for m in entries))
            self.assertTrue(all(m.uid == 0 and m.gid == 0 and m.mtime == 0 for m in entries))
            self.assertEqual(len({m.name for m in entries}), len(entries))
            packed = {m.name.removeprefix("package/"): archive.extractfile(m).read() for m in entries if m.isfile()}
        self.assertEqual(packed, self.files)
        docs.validate(packed)


if __name__ == "__main__":
    unittest.main()
