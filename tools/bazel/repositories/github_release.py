"""Download pinned HTTPS assets, with an official GitHub release API fallback."""

import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

_TIMEOUT_SECONDS = 60
_CHUNK_SIZE = 128 * 1024
_METADATA_LIMIT = 8 * 1024 * 1024
_NETWORK_ERRORS = (urllib.error.URLError, OSError, http.client.HTTPException)


class IntegrityError(ValueError):
    """Downloaded bytes do not match the pinned SHA-256."""


class DownloadError(RuntimeError):
    pass


def _retry_delay(error, attempt):
    retry_after = (error.headers or {}).get("Retry-After")
    if error.code not in (408, 429, 500, 502, 503, 504) and not (error.code == 403 and retry_after):
        return None
    if retry_after is not None:
        try:
            delay = float(retry_after)
        except ValueError:
            return None
        # Never retry earlier than requested or wait without a finite bound.
        return delay if 0 <= delay <= 60 else None
    return 60 if error.code == 429 else 2 ** attempt


def _open(url, accept):
    api = urllib.parse.urlsplit(url).netloc == "api.github.com"
    attempts = 3 if api else 1
    for attempt in range(attempts):
        headers = {"Accept": accept, "User-Agent": "GizOS-dependency-downloader"}
        if attempt:
            headers["Cache-Control"] = "no-cache"
        request = urllib.request.Request(url, headers=headers)
        if api:
            token = os.environ.get("GH_TOKEN")
            if token:
                # urllib's redirect handler copies ordinary headers only.
                request.add_unredirected_header("Authorization", "Bearer " + token)
        try:
            return urllib.request.urlopen(request, timeout=_TIMEOUT_SECONDS)
        except urllib.error.HTTPError as error:
            failure = DownloadError("HTTPS request failed (HTTP " + str(error.code) + ")")
            delay = _retry_delay(error, attempt) if api else None
            if error.fp is not None:
                error.close()
        except _NETWORK_ERRORS:
            failure = DownloadError("HTTPS request failed")
            delay = 2 ** attempt if api else None
        if delay is None or attempt + 1 == attempts:
            raise failure from None
        time.sleep(delay)


def _read(response, size):
    try:
        return response.read(size)
    except _NETWORK_ERRORS:
        raise DownloadError("HTTPS transfer failed") from None


def _copy(url, output, algorithm):
    digest = hashlib.new(algorithm)
    with _open(url, "application/octet-stream") as response:
        while True:
            chunk = _read(response, _CHUNK_SIZE)
            if not chunk:
                break
            output.write(chunk)
            digest.update(chunk)
    return digest.hexdigest()


def _release_asset(url):
    parsed = urllib.parse.urlsplit(url)
    if parsed.netloc != "github.com" or parsed.query or parsed.fragment:
        return None
    match = re.fullmatch(
        r"/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+)/releases/download/([^/]+)/([^/]+)",
        parsed.path,
    )
    if not match:
        return None
    owner, repo, tag, name = match.groups()
    if owner in (".", "..") or repo in (".", ".."):
        return None
    base = "https://api.github.com/repos/" + owner + "/" + repo
    metadata_url = base + "/releases/tags/" + urllib.parse.quote(
        urllib.parse.unquote(tag), safe=""
    )
    with _open(metadata_url, "application/vnd.github+json") as response:
        raw = bytearray()
        while len(raw) <= _METADATA_LIMIT:
            chunk = _read(response, min(_CHUNK_SIZE, _METADATA_LIMIT + 1 - len(raw)))
            if not chunk:
                break
            raw.extend(chunk)
    if len(raw) > _METADATA_LIMIT:
        raise ValueError("GitHub release metadata exceeds size limit")
    try:
        metadata = json.loads(raw)
    except (ValueError, UnicodeError):
        raise ValueError("Invalid GitHub release metadata") from None
    assets = metadata.get("assets") if isinstance(metadata, dict) else None
    if not isinstance(assets, list):
        raise ValueError("Invalid GitHub release assets")
    matches = [
        asset for asset in assets
        if isinstance(asset, dict) and asset.get("name") == urllib.parse.unquote(name)
    ]
    if len(matches) != 1:
        raise ValueError("GitHub release asset missing or ambiguous")
    asset_id = matches[0].get("id")
    if type(asset_id) is not int or asset_id <= 0:
        raise ValueError("Invalid GitHub release asset id")
    return base + "/releases/assets/" + str(asset_id)


def _checksum_spec(checksum):
    if isinstance(checksum, str):
        if re.fullmatch(r"[0-9a-fA-F]{64}", checksum):
            return "sha256", checksum.lower()
        if checksum.startswith("sha384-"):
            try:
                digest = base64.b64decode(checksum[len("sha384-"):], validate=True)
            except ValueError:
                digest = b""
            if len(digest) == 48:
                return "sha384", digest.hex()
    raise ValueError("A pinned SHA-256 digest or SHA-384 SRI is required")


def download_verified(url, sha256, destination) -> None:
    """Stream an HTTPS asset and atomically install it after pinned digest validation.

    The checksum accepts the existing SHA-256 hex form or an upstream SHA-384 SRI.

    Reuses an existing matching regular, nonsymlink destination without network.
    Raises IntegrityError for mismatches, ValueError for invalid inputs or metadata,
    RuntimeError for network failures, and OSError for local filesystem failures.
    Each request has a finite socket timeout. Transient API request failures
    get at most three attempts; integrity and permanent HTTP errors are not retried.
    Existing destinations survive
    failure, and only this invocation's same-directory temporary file is removed.
    """
    algorithm, expected = _checksum_spec(sha256)
    try:
        parsed = urllib.parse.urlsplit(url)
        valid = (parsed.scheme == "https" and parsed.hostname
                 and parsed.username is None and parsed.password is None)
    except ValueError:
        valid = False
    if not valid:
        raise ValueError("An HTTPS URL without credentials is required")
    destination = Path(destination)
    if not destination.is_symlink() and destination.is_file():
        try:
            digest = hashlib.new(algorithm)
            with destination.open("rb") as cached:
                for chunk in iter(lambda: cached.read(_CHUNK_SIZE), b""):
                    digest.update(chunk)
            if digest.hexdigest() == expected:
                return
        except OSError:
            pass
    staged = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent, prefix=".github-release-", delete=False
        ) as output:
            staged = output.name
            try:
                actual = _copy(url, output, algorithm)
            except DownloadError:
                fallback = _release_asset(url)
                if fallback is None:
                    raise DownloadError("HTTPS download failed") from None
                output.seek(0)
                output.truncate()
                actual = _copy(fallback, output, algorithm)
            if actual != expected:
                label = "SHA-256" if algorithm == "sha256" else "SHA-384"
                raise IntegrityError("Downloaded asset " + label + " mismatch")
        os.replace(staged, destination)
    finally:
        if staged is not None:
            try:
                os.unlink(staged)
            except FileNotFoundError:
                pass


def main():
    if len(sys.argv) != 4:
        print("Usage: github_release.py URL CHECKSUM DESTINATION", file=sys.stderr)
        return 2
    try:
        download_verified(*sys.argv[1:])
    except DownloadError as error:
        # Transport messages contain only controlled text and HTTP status codes.
        print("Verified asset download failed: " + str(error), file=sys.stderr)
        return 1
    except (ValueError, RuntimeError, OSError):
        # Do not expose exception details containing URLs, headers or credentials.
        print("Verified asset download failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
