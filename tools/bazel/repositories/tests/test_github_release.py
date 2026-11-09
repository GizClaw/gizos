"""Offline behavior tests; runnable directly with python3 -I."""

import base64
import hashlib
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error
import urllib.request

sys.dont_write_bytecode = True
_SCRIPT = Path(__file__).resolve().parents[1] / "github_release.py"
_SPEC = importlib.util.spec_from_file_location("github_release", _SCRIPT)
release = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(release)

_DATA = b"verified archive bytes\x00" * 10000
_HASH = hashlib.sha256(_DATA).hexdigest()
_URL = "https://github.com/owner/repo/releases/download/v1%2Ftest/archive%20name.zip"
_METADATA_URL = "https://api.github.com/repos/owner/repo/releases/tags/v1%2Ftest"
_ASSET_URL = "https://api.github.com/repos/owner/repo/releases/assets/42"


def metadata(assets=None):
    if assets is None:
        assets = [{"name": "archive name.zip", "id": 42,
                   "url": "https://untrusted.invalid/asset"}]
    return io.BytesIO(json.dumps({"assets": assets}).encode())


def unavailable():
    return urllib.error.HTTPError(_URL, 504, "Gateway timeout", {}, None)


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.destination = Path(self.directory.name) / "asset.zip"
        self.destination.write_bytes(b"old destination")
        self.urlopen = mock.patch.object(release.urllib.request, "urlopen").start()
        self.sleep = mock.patch.object(release.time, "sleep").start()
        self.addCleanup(mock.patch.stopall)
        self.environment = mock.patch.dict(os.environ, {}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def assert_clean(self, expected=b"old destination"):
        self.assertEqual(self.destination.read_bytes(), expected)
        self.assertEqual(list(Path(self.directory.name).iterdir()), [self.destination])

    def test_direct_success_streams_and_replaces(self):
        response = mock.MagicMock(wraps=io.BytesIO(_DATA))
        response.__enter__.return_value = response
        self.urlopen.return_value = response
        release.download_verified("https://example.org/archive", _HASH.upper(), self.destination)
        self.assert_clean(_DATA)
        self.assertGreater(response.read.call_count, 2)
        for call in response.read.call_args_list:
            self.assertGreater(call.args[0], 0)
            self.assertLessEqual(call.args[0], 128 * 1024)
        self.assertEqual(self.urlopen.call_count, 1)
        self.assertGreater(self.urlopen.call_args.kwargs["timeout"], 0)

    def test_matching_cache_is_reused_without_network(self):
        self.destination.write_bytes(_DATA)
        release.download_verified(_URL, _HASH.upper(), self.destination)
        self.urlopen.assert_not_called()
        self.assert_clean(_DATA)

    def test_symlink_is_not_reused_or_followed_for_installation(self):
        target = Path(self.directory.name) / "target.zip"
        target.write_bytes(_DATA)
        self.destination.unlink()
        self.destination.symlink_to(target)
        self.urlopen.return_value = io.BytesIO(_DATA)
        release.download_verified(_URL, _HASH, self.destination)
        self.urlopen.assert_called_once()
        self.assertFalse(self.destination.is_symlink())
        self.assertEqual(target.read_bytes(), _DATA)
        self.assertEqual(self.destination.read_bytes(), _DATA)
        self.assertEqual(set(Path(self.directory.name).iterdir()), {target, self.destination})

    def test_github_504_api_fallback(self):
        self.urlopen.side_effect = [unavailable(), metadata(), io.BytesIO(_DATA)]
        release.download_verified(_URL, _HASH, self.destination)
        self.assert_clean(_DATA)
        calls = self.urlopen.call_args_list
        self.assertEqual([call.args[0].full_url for call in calls],
                         [_URL, _METADATA_URL, _ASSET_URL])
        self.assertEqual(calls[2].args[0].get_header("Accept"), "application/octet-stream")
        self.assertTrue(all(0 < call.kwargs["timeout"] <= 60 for call in calls))

    def test_checksum_mismatch_never_falls_back(self):
        self.urlopen.return_value = io.BytesIO(b"wrong contents")
        with self.assertRaisesRegex(release.IntegrityError, "SHA-256 mismatch"):
            release.download_verified(_URL, _HASH, self.destination)
        self.assertEqual(self.urlopen.call_count, 1)
        self.assert_clean()

    def test_sha384_sri_and_matching_cache_preserve_the_upstream_algorithm(self):
        integrity = "sha384-" + base64.b64encode(hashlib.sha384(_DATA).digest()).decode()
        self.urlopen.return_value = io.BytesIO(_DATA)
        release.download_verified(_URL, integrity, self.destination)
        self.assert_clean(_DATA)
        self.urlopen.reset_mock()
        release.download_verified(_URL, integrity, self.destination)
        self.urlopen.assert_not_called()

    def test_sha384_mismatch_never_falls_back(self):
        integrity = "sha384-" + base64.b64encode(hashlib.sha384(_DATA).digest()).decode()
        self.urlopen.return_value = io.BytesIO(b"wrong contents")
        with self.assertRaisesRegex(release.IntegrityError, "SHA-384 mismatch"):
            release.download_verified(_URL, integrity, self.destination)
        self.assertEqual(self.urlopen.call_count, 1)
        self.assert_clean()

    def test_invalid_sha384_integrity_is_rejected_before_io(self):
        for integrity in ("sha384-", "sha384-invalid!", "sha384-eA==", "sha1-eA=="):
            with self.subTest(integrity=integrity):
                with self.assertRaises(ValueError):
                    release.download_verified(_URL, integrity, self.destination)
                self.urlopen.assert_not_called()
                self.assert_clean()

    def test_invalid_or_missing_asset(self):
        for assets in ([], [{"name": "other", "id": 42}],
                       [{"name": "archive name.zip", "id": True}],
                       [{"name": "archive name.zip", "id": "42"}],
                       [{"name": "archive name.zip", "id": 0}],
                       [{"name": "archive name.zip", "id": -1}],
                       [{"name": "archive name.zip"}],
                       [{"name": "archive name.zip", "id": 42}] * 2):
            with self.subTest(assets=assets):
                self.urlopen.reset_mock()
                self.urlopen.side_effect = [unavailable(), metadata(assets)]
                with self.assertRaises(ValueError):
                    release.download_verified(_URL, _HASH, self.destination)
                self.assertEqual(self.urlopen.call_count, 2)
                self.assert_clean()

    def test_interrupted_transfer_cleanup(self):
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.read.side_effect = [b"partial", OSError("signed URL secret")]
        self.urlopen.return_value = response
        with self.assertRaisesRegex(RuntimeError, "^HTTPS download failed$"):
            release.download_verified("https://example.org/archive", _HASH, self.destination)
        self.assertEqual(self.urlopen.call_count, 1)
        self.assert_clean()

    def test_interrupted_original_is_truncated_before_fallback(self):
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.read.side_effect = [b"partial" * 100000,
                                     http.client.IncompleteRead(b"more")]
        self.urlopen.side_effect = [response, metadata(), io.BytesIO(_DATA)]
        release.download_verified(_URL, _HASH, self.destination)
        self.assert_clean(_DATA)

    def test_transient_api_http_error_retries_then_succeeds(self):
        failure = urllib.error.HTTPError(_METADATA_URL, 504, "private redirect", {}, None)
        self.urlopen.side_effect = [unavailable(), failure, metadata(), io.BytesIO(_DATA)]
        release.download_verified(_URL, _HASH, self.destination)
        self.assert_clean(_DATA)
        self.assertEqual(self.urlopen.call_count, 4)
        self.sleep.assert_called_once_with(1)
        self.assertEqual(self.urlopen.call_args_list[2].args[0].get_header("Cache-control"), "no-cache")

    def test_api_network_error_retries_then_succeeds(self):
        self.urlopen.side_effect = [unavailable(), urllib.error.URLError("private signed URL"), metadata(), io.BytesIO(_DATA)]
        release.download_verified(_URL, _HASH, self.destination)
        self.assert_clean(_DATA)
        self.sleep.assert_called_once_with(1)

    def test_api_retry_exhaustion_preserves_destination_and_redacts_details(self):
        failure = urllib.error.HTTPError(_METADATA_URL, 503, "private signed URL", {}, None)
        self.urlopen.side_effect = [unavailable(), failure, failure, failure]
        with self.assertRaisesRegex(release.DownloadError, "HTTP 503") as error:
            release.download_verified(_URL, _HASH, self.destination)
        self.assertNotIn("private", str(error.exception))
        self.assertEqual(self.urlopen.call_count, 4)
        self.assertEqual(self.sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.assert_clean()

    def test_permanent_api_http_error_is_not_retried(self):
        for code in (401, 403, 404):
            with self.subTest(code=code):
                self.urlopen.reset_mock()
                self.sleep.reset_mock()
                failure = urllib.error.HTTPError(_METADATA_URL, code, "private URL", {}, None)
                self.urlopen.side_effect = [unavailable(), failure]
                with self.assertRaisesRegex(release.DownloadError, "HTTP " + str(code)):
                    release.download_verified(_URL, _HASH, self.destination)
                self.assertEqual(self.urlopen.call_count, 2)
                self.sleep.assert_not_called()
                self.assert_clean()

    def test_retry_after_is_respected(self):
        failure = urllib.error.HTTPError(_METADATA_URL, 429, "rate limited", {"Retry-After": "7"}, None)
        self.urlopen.side_effect = [unavailable(), failure, metadata(), io.BytesIO(_DATA)]
        release.download_verified(_URL, _HASH, self.destination)
        self.sleep.assert_called_once_with(7)
        self.assert_clean(_DATA)

    def test_unbounded_retry_after_is_not_shortened_or_waited(self):
        failure = urllib.error.HTTPError(_METADATA_URL, 429, "rate limited", {"Retry-After": "120"}, None)
        self.urlopen.side_effect = [unavailable(), failure]
        with self.assertRaisesRegex(release.DownloadError, "HTTP 429"):
            release.download_verified(_URL, _HASH, self.destination)
        self.assertEqual(self.urlopen.call_count, 2)
        self.sleep.assert_not_called()
        self.assert_clean()

    def test_metadata_failures_cleanup(self):
        for failure in (urllib.error.URLError("secret signed URL"),
                        io.BytesIO(b"not json"), io.BytesIO(b"[]"),
                        io.BytesIO(b'{"assets": {}}'),
                        io.BytesIO(b"x" * (release._METADATA_LIMIT + 1))):
            with self.subTest(failure=type(failure).__name__):
                attempts = [failure] * 3 if isinstance(failure, urllib.error.URLError) else [failure]
                self.urlopen.side_effect = [unavailable(), *attempts]
                with self.assertRaises((RuntimeError, ValueError)):
                    release.download_verified(_URL, _HASH, self.destination)
                self.assert_clean()

    def test_token_only_on_api_and_not_forwarded_on_redirect(self):
        os.environ["GH_TOKEN"] = "test-secret-token"
        self.urlopen.side_effect = [unavailable(), metadata(), io.BytesIO(_DATA)]
        release.download_verified(_URL, _HASH, self.destination)
        requests = [call.args[0] for call in self.urlopen.call_args_list]
        self.assertIsNone(requests[0].get_header("Authorization"))
        for request in requests[1:]:
            self.assertEqual(request.get_header("Authorization"), "Bearer test-secret-token")
            self.assertNotIn("Authorization", request.headers)
            self.assertIn("Authorization", request.unredirected_hdrs)
            redirected = urllib.request.HTTPRedirectHandler().redirect_request(
                request, None, 302, "Found", {}, "https://objects.example/archive?signature=secret"
            )
            self.assertIsNone(redirected.get_header("Authorization"))
        for url in ("https://example.org/archive", "https://api.github.com.evil.invalid/archive",
                    "https://github.com.evil.invalid/owner/repo/releases/download/v1/a"):
            self.destination.write_bytes(b"old destination")
            self.urlopen.reset_mock()
            self.urlopen.side_effect = None
            self.urlopen.return_value = io.BytesIO(_DATA)
            release.download_verified(url, _HASH, self.destination)
            self.assertIsNone(self.urlopen.call_args.args[0].get_header("Authorization"))

    def test_non_exact_github_hosts_do_not_fall_back(self):
        for url in ("https://github.com.evil.invalid/o/r/releases/download/v/a",
                    "https://github.com:443/o/r/releases/download/v/a",
                    "https://github.com/o/r/releases/download/v/a?query=yes"):
            self.urlopen.reset_mock()
            self.urlopen.side_effect = urllib.error.URLError("private redirect URL")
            with self.assertRaisesRegex(RuntimeError, "^HTTPS download failed$"):
                release.download_verified(url, _HASH, self.destination)
            self.assertEqual(self.urlopen.call_count, 1)
            self.assert_clean()

    def test_invalid_hash_and_insecure_urls_before_io(self):
        for digest in ("", "0" * 63, "g" * 64, "0" * 64 + "\n", None):
            with self.subTest(digest=digest):
                with self.assertRaises(ValueError):
                    release.download_verified(_URL, digest, self.destination)
        for url in ("http://github.com/o/r/releases/download/v/a", "file:///tmp/asset",
                    "https://user:password@api.github.com/archive", "https:///archive"):
            with self.subTest(url=url):
                with self.assertRaises(ValueError):
                    release.download_verified(url, _HASH, self.destination)
        self.urlopen.assert_not_called()
        self.assert_clean()

    def test_cli_isolated_invalid_input_has_generic_error(self):
        result = subprocess.run(
            [sys.executable, "-I", str(_SCRIPT), "https://example.org/secret", "bad",
             str(self.destination)], capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "Verified asset download failed\n")
        self.assert_clean()


if __name__ == "__main__":
    unittest.main()
