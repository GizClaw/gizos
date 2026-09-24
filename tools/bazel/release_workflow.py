"""Validate the manual Release snapshot and its downloaded assets."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


class ReleaseWorkflowError(ValueError):
    pass


def require_snapshot(ref: str, default_branch: str, commit: str, checkout: str) -> None:
    if ref != f"refs/heads/{default_branch}" or checkout != commit:
        raise ReleaseWorkflowError("Release must use the default branch trigger commit")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def asset(path: Path) -> dict[str, object]:
    return {"name": path.name, "size": path.stat().st_size, "sha256": sha256(path)}


def checked_assets(directory: Path, *, expect_index: bool) -> list[Path]:
    files = {path.name: path for path in directory.iterdir() if path.is_file() and not path.is_symlink()}
    if any(path.is_symlink() or not path.is_file() for path in directory.iterdir()):
        raise ReleaseWorkflowError("Release assets must be regular files")
    checksums = files.get("SHA256SUMS")
    if checksums is None:
        raise ReleaseWorkflowError("SHA256SUMS is missing")
    listed: dict[str, str] = {}
    for line in checksums.read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+-]*)", line)
        if match is None or match[2] in listed or match[2] in {"SHA256SUMS", "index.json"}:
            raise ReleaseWorkflowError("SHA256SUMS has an invalid entry")
        listed[match[2]] = match[1]
    if not listed:
        raise ReleaseWorkflowError("SHA256SUMS is empty")
    expected = set(listed) | {"SHA256SUMS"}
    if expect_index:
        expected.add("index.json")
    if set(files) != expected:
        raise ReleaseWorkflowError("Release asset set differs from SHA256SUMS")
    for name, digest in listed.items():
        if sha256(files[name]) != digest:
            raise ReleaseWorkflowError(f"Release asset checksum mismatch: {name}")
    return [files[name] for name in sorted(files) if name != "index.json"]


def validate_gizos_release(directory: Path, tag: str, commit: str) -> None:
    path = directory / "gizos-release.json"
    if not path.exists():
        return
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseWorkflowError(f"invalid GizOS release metadata: {error}") from error
    if (
        not isinstance(metadata, dict)
        or metadata.get("release_id") != tag.removeprefix("v")
        or metadata.get("release_tag") != tag
        or metadata.get("commit") != commit
    ):
        raise ReleaseWorkflowError("GizOS release metadata differs from trigger identity")


def prepare(directory: Path, repository: str, tag: str, commit: str) -> None:
    assets = checked_assets(directory, expect_index=False)
    validate_gizos_release(directory, tag, commit)
    index = {
        "repository": repository,
        "release_tag": tag,
        "commit": commit,
        "assets": [asset(path) for path in assets],
    }
    (directory / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")


def verify(directory: Path, repository: str, tag: str, commit: str) -> None:
    assets = checked_assets(directory, expect_index=True)
    validate_gizos_release(directory, tag, commit)
    expected = {
        "repository": repository,
        "release_tag": tag,
        "commit": commit,
        "assets": [asset(path) for path in assets],
    }
    if json.loads((directory / "index.json").read_text(encoding="utf-8")) != expected:
        raise ReleaseWorkflowError("Release index does not match downloaded assets")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["gate", "prepare", "verify"])
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--repository")
    parser.add_argument("--tag")
    parser.add_argument("--commit")
    parser.add_argument("--ref")
    parser.add_argument("--default-branch")
    args = parser.parse_args()
    try:
        if args.mode == "gate":
            checkout = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
            require_snapshot(args.ref, args.default_branch, args.commit, checkout)
        elif args.mode == "prepare":
            prepare(args.directory, args.repository, args.tag, args.commit)
        else:
            verify(args.directory, args.repository, args.tag, args.commit)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"release validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
