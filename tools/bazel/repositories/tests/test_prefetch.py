"""Exercise locked archive prefetching without network access."""

import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


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
        if url not in self.payloads:
            raise prefetch.DownloadError("HTTP 404")
        data = self.payloads[url]
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError("Integrity mismatch")
        Path(destination).write_bytes(data)

    def run_prefetch(self, entries):
        self.lock.write_text(json.dumps({"registryFileHashes": dict(entries)}))
        return prefetch.prefetch(self.lock, self.distdir, self.download)

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

    def test_other_archive_transports_are_left_to_bazel(self):
        entry = self.metadata("other", "https://example.invalid/archive.tar.gz", b"archive")
        self.assertEqual(self.run_prefetch([entry]), 0)
        self.assertEqual(self.calls, [entry[0]])


if __name__ == "__main__":
    unittest.main()
