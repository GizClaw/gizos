"""Exercise locked archive prefetching without network access."""

import base64
import hashlib
import importlib.util
import io
import json
import tarfile
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SOURCE = Path(__file__).resolve().parents[1] / "prefetch.py"
spec = importlib.util.spec_from_file_location("prefetch", SOURCE)
prefetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prefetch)


class PrefetchTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.lock = self.root / "MODULE.bazel.lock"
        self.distdir = self.root / "distdir"
        self.payloads = {}
        self.calls = []
        self.checksums = []

    def metadata(self, name, archive, payload):
        url = "https://bcr.bazel.build/modules/" + name + "/1.0/source.json"
        data = json.dumps({
            "url": archive,
            "integrity": "sha256-" + base64.b64encode(hashlib.sha256(payload).digest()).decode(),
        }).encode()
        self.payloads[url] = data
        return url, hashlib.sha256(data).hexdigest()

    def download(self, url, digest, destination):
        self.calls.append(url)
        self.checksums.append((url, digest))
        if url not in self.payloads:
            raise prefetch.DownloadError("HTTP 404")
        data = self.payloads[url]
        actual = hashlib.sha256(data).hexdigest()
        if digest.startswith("sha384-"):
            actual = "sha384-" + base64.b64encode(hashlib.sha384(data).digest()).decode()
        if actual != digest:
            raise ValueError("Integrity mismatch")
        Path(destination).write_bytes(data)

    def run_prefetch(self, entries, extensions=None, host=None):
        self.lock.write_text(json.dumps({
            "registryFileHashes": dict(entries), "moduleExtensions": extensions or {},
        }))
        return prefetch.prefetch(self.lock, self.distdir, self.download, host)

    def test_import_and_explicit_cache_work_without_a_home_directory(self):
        self.lock.write_text(json.dumps({"registryFileHashes": {}}))
        isolated_spec = importlib.util.spec_from_file_location("prefetch_without_home", SOURCE)
        isolated = importlib.util.module_from_spec(isolated_spec)
        with mock.patch.object(Path, "home", side_effect=RuntimeError("no home directory")) as home:
            isolated_spec.loader.exec_module(isolated)
            self.assertEqual(isolated.prefetch(self.lock, self.distdir, self.download), 0)
            home.assert_not_called()
            with self.assertRaisesRegex(RuntimeError, "no home directory"):
                isolated.prefetch(self.lock)

    def test_mirror_archive_has_exact_bytes_and_keeps_original_pin(self):
        original = "https://github.com/example/rules/releases/download/v1/rules.tar.gz"
        mirror = original.replace("https://github.com/", "https://mirror.bazel.build/github.com/")
        entry = self.metadata("rules", original, b"pinned archive")
        self.payloads[mirror] = b"pinned archive"
        self.assertEqual(self.run_prefetch([entry]), 1)
        self.assertEqual((self.distdir / "rules.tar.gz").read_bytes(), b"pinned archive")
        self.assertNotIn(original, self.calls)

    def test_missing_mirror_uses_original_download_helper(self):
        original = "https://github.com/example/rules/releases/download/v1/rules.tar.gz"
        entry = self.metadata("rules", original, b"pinned archive")
        self.payloads[original] = b"pinned archive"
        self.assertEqual(self.run_prefetch([entry]), 1)
        self.assertEqual((self.distdir / "rules.tar.gz").read_bytes(), b"pinned archive")
        self.assertEqual(self.calls[-1], original)

    def test_wrong_mirror_bytes_fail_without_retrying_another_origin(self):
        original = "https://github.com/example/rules/releases/download/v1/rules.tar.gz"
        mirror = original.replace("https://github.com/", "https://mirror.bazel.build/github.com/")
        entry = self.metadata("rules", original, b"pinned archive")
        self.payloads[mirror] = b"wrong bytes"
        self.payloads[original] = b"pinned archive"
        with self.assertRaisesRegex(ValueError, "Integrity mismatch"):
            self.run_prefetch([entry])
        self.assertNotIn(original, self.calls)
        self.assertFalse((self.distdir / "rules.tar.gz").exists())

    def test_registry_metadata_must_match_the_lock(self):
        original = "https://github.com/example/rules/releases/download/v1/rules.tar.gz"
        url, digest = self.metadata("rules", original, b"pinned archive")
        self.payloads[url] += b" "
        with self.assertRaisesRegex(ValueError, "Integrity mismatch"):
            self.run_prefetch([(url, digest)])
        self.assertEqual(self.calls, [url])

    def test_unsafe_filename_cannot_escape_the_distdir(self):
        original = "https://github.com/example/rules/releases/download/v1/%2e%2e%2fescape"
        entry = self.metadata("rules", original, b"pinned archive")
        with self.assertRaisesRegex(ValueError, "filename"):
            self.run_prefetch([entry])
        self.assertFalse((self.root / "escape").exists())

    def test_colliding_names_fail_before_archives_are_downloaded(self):
        first = self.metadata("first", "https://github.com/one/rules/releases/download/v1/source.tar.gz", b"one")
        second = self.metadata("second", "https://github.com/two/rules/releases/download/v1/source.tar.gz", b"two")
        with self.assertRaisesRegex(ValueError, "collide"):
            self.run_prefetch([first, second])
        self.assertEqual(set(self.calls), {first[0], second[0]})

    def test_shared_archive_is_downloaded_once(self):
        archive = "https://github.com/example/rules/releases/download/v1/rules.tar.gz"
        mirror = archive.replace("https://github.com/", "https://mirror.bazel.build/github.com/")
        first = self.metadata("first", archive, b"pinned archive")
        second = self.metadata("second", archive, b"pinned archive")
        self.payloads[mirror] = b"pinned archive"
        self.assertEqual(self.run_prefetch([first, second]), 1)
        self.assertEqual(self.calls.count(mirror), 1)
        self.assertEqual(self.calls.count(first[0]) + self.calls.count(second[0]), 1)

    def test_other_archive_transports_are_left_to_bazel(self):
        entry = self.metadata("other", "https://example.invalid/archive.tar.gz", b"archive")
        self.assertEqual(self.run_prefetch([entry]), 0)
        self.assertEqual(self.calls, [entry[0]])

    def bun_fixture(self, archive="bun-linux-x64-baseline"):
        data = b"pinned Bun archive"
        digest = hashlib.sha256(data).hexdigest()
        manifest = self.root / "tools/gizos/bun_runtime.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps({
            "version": "1.3.9",
            "url": "https://github.com/oven-sh/bun/releases/download/bun-v{version}/{archive}.zip",
            "archives": {"linux-x64": {"archive": archive, "sha256": digest}},
        }))
        url = "https://github.com/oven-sh/bun/releases/download/bun-v1.3.9/" + archive + ".zip"
        self.payloads[url] = data
        return url, digest

    def test_bun_host_uses_the_single_owner_manifest(self):
        url, digest = self.bun_fixture()
        self.assertEqual(self.run_prefetch([], host="linux_amd64"), 1)
        self.assertEqual((self.distdir / "bun-linux-x64-baseline.zip").read_bytes(), self.payloads[url])
        self.assertIn((url, digest), self.checksums)

    def test_bun_archive_mismatch_fails_without_installation(self):
        url, digest = self.bun_fixture()
        self.payloads[url] = b"wrong archive"
        with self.assertRaisesRegex(ValueError, "Integrity mismatch"):
            self.run_prefetch([], host="linux_amd64")
        self.assertFalse((self.distdir / "bun-linux-x64-baseline.zip").exists())

    def test_bun_unsupported_host_is_rejected_before_network(self):
        self.bun_fixture()
        with self.assertRaisesRegex(ValueError, "Unsupported pinned Bun host"):
            self.run_prefetch([], host="windows_arm64")
        self.assertFalse(self.calls)

    def test_bun_manifest_cannot_escape_the_distdir(self):
        self.bun_fixture("../../escape")
        with self.assertRaisesRegex(ValueError, "filename"):
            self.run_prefetch([], host="linux_amd64")
        self.assertFalse(self.calls)
        self.assertFalse((self.root / "escape.zip").exists())

    def yq_fixture(self, contents=None):
        tool = b"pinned yq executable bytes"
        integrity = "sha384-" + base64.b64encode(hashlib.sha384(tool).digest()).decode()
        if contents is None:
            contents = "YQ_VERSIONS = " + repr({"4.45.2": {"linux_amd64": integrity}})
            contents += "\nraise RuntimeError('vendor code must not execute')\n"
        contents = contents.encode()
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
            member = tarfile.TarInfo("upstream/yq/toolchain/versions.bzl")
            member.size = len(contents)
            archive.addfile(member, io.BytesIO(contents))
        data = buffer.getvalue()
        source = "https://github.com/example/yq.bzl/releases/download/v1/yq.tar.gz"
        entry = self.metadata("yq.bzl", source, data)
        self.payloads[source.replace("https://github.com/", "https://mirror.bazel.build/github.com/")] = data
        url = "https://github.com/mikefarah/yq/releases/download/v4.45.2/yq_linux_amd64"
        self.payloads[url] = tool
        extensions = {"yq": {"general": {"generatedRepoSpecs": {"host": {
            "repoRuleId": "@@yq.bzl+//yq/toolchain:platforms.bzl%yq_platform_repo",
            "attributes": {"platform": "linux_amd64", "version": "4.45.2"},
        }}}}}
        return entry, extensions, url, integrity

    def test_yq_host_pin_is_read_as_literal_data_without_execution(self):
        entry, extensions, url, integrity = self.yq_fixture()
        self.assertEqual(self.run_prefetch([entry], extensions, "linux_amd64"), 2)
        self.assertEqual((self.distdir / "yq_linux_amd64").read_bytes(), self.payloads[url])
        self.assertIn((url, integrity), self.checksums)

    def test_yq_nonliteral_table_is_not_executed(self):
        entry, extensions, url, integrity = self.yq_fixture("YQ_VERSIONS = __import__('os').getcwd()")
        with self.assertRaises(ValueError):
            self.run_prefetch([entry], extensions, "linux_amd64")
        self.assertNotIn(url, self.calls)

    def test_yq_wrong_bytes_fail_the_original_sha384_pin(self):
        entry, extensions, url, integrity = self.yq_fixture()
        self.payloads[url] = b"corrupt executable"
        with self.assertRaisesRegex(ValueError, "Integrity mismatch"):
            self.run_prefetch([entry], extensions, "linux_amd64")
        self.assertFalse((self.distdir / "yq_linux_amd64").exists())

    def test_yq_other_host_is_not_downloaded(self):
        entry, extensions, url, integrity = self.yq_fixture()
        self.assertEqual(self.run_prefetch([entry], extensions, "darwin_arm64"), 1)
        self.assertNotIn(url, self.calls)


if __name__ == "__main__":
    unittest.main()
