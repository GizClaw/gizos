"""Publish the exact gizos tarball produced by a successful GizOS Release."""

from __future__ import annotations

import argparse
import base64
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import time


PACKAGE = "gizos"
REGISTRY = "https://registry.npmjs.org"


def release_version(batch: str) -> str:
    if not re.fullmatch(r"\d{8}-\d{6}", batch):
        raise ValueError("release batch must be YYYYMMDD-HHMMSS")
    date = datetime.strptime(batch, "%Y%m%d-%H%M%S")
    return f"{date.year}.{date.month}.{date.day}-r{batch[-6:]}"


def checked_tarball(directory: Path, batch: str) -> tuple[Path, str, str]:
    index = json.loads((directory / "npm-index.json").read_text())
    if index.get("format") != 1 or index.get("version") != batch:
        raise ValueError("npm index does not belong to this release")
    packages = index.get("packages", [])
    candidates = [item for item in packages if item.get("name") == PACKAGE]
    if len(candidates) != 1:
        raise ValueError("npm index must contain one gizos package")
    item = candidates[0]
    version = release_version(batch)
    name = f"gizos-{version}.tgz"
    if item.get("version") != version or item.get("tarball") != name:
        raise ValueError("gizos package version must match the release batch")
    path = directory / name
    if path.is_symlink() or not path.is_file():
        raise ValueError("gizos tarball is missing or not a regular file")
    content = path.read_bytes()
    if len(content) != item.get("size") or hashlib.sha256(content).hexdigest() != item.get("sha256"):
        raise ValueError("gizos tarball does not match its release checksum")
    with tarfile.open(path) as archive:
        manifest = json.load(archive.extractfile("package/package.json"))
        if manifest.get("name") != PACKAGE or manifest.get("version") != version:
            raise ValueError("tarball package identity differs from the release index")
        if manifest.get("publishConfig", {}).get("registry") != REGISTRY:
            raise ValueError("gizos must publish to the public npm registry")
        if not archive.getmember("package/bin/gizos.js").mode & 0o111:
            raise ValueError("gizos npm entry is not executable")
    integrity = "sha512-" + base64.b64encode(hashlib.sha512(content).digest()).decode()
    return path, version, integrity


def command(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, capture_output=True, text=True, check=False)


def published_integrity(version: str, run=command) -> str | None:
    result = run(["npm", "view", f"{PACKAGE}@{version}", "dist.integrity", "--json", "--registry", REGISTRY])
    if result.returncode == 0:
        value = json.loads(result.stdout)
        if not isinstance(value, str):
            raise ValueError("npm returned an invalid package integrity")
        return value
    try:
        missing = json.loads(result.stdout).get("error", {}).get("code") == "E404"
    except (ValueError, AttributeError):
        missing = False
    if missing:
        return None
    raise ValueError(f"cannot query npm package: {result.stderr.strip()}")


def publish(directory: Path, batch: str, run=command) -> str:
    path, version, expected = checked_tarball(directory, batch)
    current = published_integrity(version, run)
    if current is not None:
        if current != expected:
            raise ValueError("published npm version has different content")
        return f"{PACKAGE}@{version} is already published with matching content"
    result = run([
        "npm", "publish", str(path), "--access", "public", "--provenance",
        "--ignore-scripts", "--tag", "latest", "--registry", REGISTRY,
    ])
    if result.returncode:
        raise ValueError(f"npm publication failed: {result.stderr.strip()}")
    for attempt in range(10):
        current = published_integrity(version, run)
        if current == expected:
            return f"Published and verified {PACKAGE}@{version}"
        if current is not None:
            raise ValueError("published npm package integrity does not match the release")
        if attempt < 9:
            time.sleep(2)
    raise ValueError("npm did not expose the published package integrity")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--batch", required=True)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    if args.check_only:
        print(checked_tarball(args.directory, args.batch)[0])
    else:
        print(publish(args.directory, args.batch))
