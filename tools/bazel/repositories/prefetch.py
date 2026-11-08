#!/usr/bin/env python3
"""Prefetch locked GitHub release archives into Bazel's standard distdir."""

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path, PureWindowsPath
import re
import sys
from urllib.parse import unquote, urlsplit

# Only this first-party directory is added; downloaded archives are never imported.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from github_release import DownloadError, download_verified
from toolchain_assets import prefetch_bun, prefetch_yq


DEFAULT_DIRECTORY = Path(".cache/bazel/distdir")


def prefetch(lockfile, directory=None, download=download_verified, host=None):
    directory = Path(directory) if directory is not None else Path.home() / DEFAULT_DIRECTORY
    metadata_directory = directory / ".registry"
    metadata_directory.mkdir(parents=True, exist_ok=True)
    lock = json.loads(Path(lockfile).read_text())
    registry = lock["registryFileHashes"]
    metadata = [
        (url, digest)
        for url, digest in registry.items()
        if url.endswith("/source.json") and digest is not None
    ]

    def resolve(item):
        url, digest = item
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Registry metadata requires a pinned SHA-256")
        cached = metadata_directory / (digest + ".json")
        download(url, digest, cached)
        source = json.loads(cached.read_text())
        archive = source.get("url", "")
        parsed = urlsplit(archive)
        if (
            parsed.scheme != "https"
            or parsed.netloc != "github.com"
            or not re.fullmatch(r"/[^/]+/[^/]+/releases/download/.+/.+", parsed.path)
        ):
            return None
        name = unquote(parsed.path.rsplit("/", 1)[1])
        if (
            name in ("", ".", "..")
            or "/" in name
            or "\\" in name
            or PureWindowsPath(name).name != name
        ):
            raise ValueError("Archive filename must stay inside the distdir")
        integrity = source.get("integrity", "")
        if not isinstance(integrity, str) or not integrity.startswith("sha256-"):
            raise ValueError("GitHub archive requires a pinned SHA-256 integrity")
        checksum = base64.b64decode(integrity[len("sha256-"):], validate=True)
        if len(checksum) != 32:
            raise ValueError("Invalid archive SHA-256 integrity")
        return archive, checksum.hex(), name

    with ThreadPoolExecutor(max_workers=4) as workers:
        archives = [item for item in workers.map(resolve, metadata) if item is not None]

    names = {}
    unique = []
    for archive, digest, name in archives:
        if name in names:
            if names[name] != digest:
                raise ValueError("Locked archives collide on distdir filename: " + name)
            continue
        names[name] = digest
        unique.append((archive, digest, name))

    def fetch(item):
        archive, digest, name = item
        destination = directory / name
        mirror = "https://mirror.bazel.build/github.com/" + archive[len("https://github.com/"):]
        try:
            download(mirror, digest, destination)
        except DownloadError:
            download(archive, digest, destination)
        return name

    with ThreadPoolExecutor(max_workers=4) as workers:
        for name in workers.map(fetch, unique):
            print("Verified Bazel distfile: " + name, flush=True)
    tools = prefetch_yq(lock, directory, download, host)
    tools += prefetch_bun(Path(lockfile).resolve().parent / "tools/gizos/bun_runtime.json", directory, download, host)
    return len(unique) + tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lockfile", type=Path, default=Path("MODULE.bazel.lock"))
    parser.add_argument("--directory", type=Path)
    arguments = parser.parse_args()
    prefetch(arguments.lockfile, arguments.directory)


if __name__ == "__main__":
    main()
