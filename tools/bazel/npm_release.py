#!/usr/bin/env python3
"""Pack exact manifest file lists as normalized PAX tar + filename-free gzip."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tarfile


NAME_PATTERN = re.compile(r"(?:@[a-z0-9][a-z0-9._-]*/)?[a-z0-9][a-z0-9._-]*")
VERSION_PATTERN = re.compile(
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
)


def read_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise ValueError("package.json must be an object")
    for key, pattern in (("name", NAME_PATTERN), ("version", VERSION_PATTERN)):
        value = manifest.get(key)
        if not isinstance(value, str) or not pattern.fullmatch(value):
            raise ValueError(f"package.json has missing or invalid {key}: {value!r}")
    return manifest


def tarball_name(manifest: dict) -> str:
    # npm pack: @scope/name -> scope-name; unscoped names stay unchanged.
    name = manifest["name"].removeprefix("@").replace("/", "-")
    return f"{name}-{manifest['version']}.tgz"


def manifest_files(manifest: dict) -> set[str]:
    files = manifest.get("files")
    if not isinstance(files, list):
        raise ValueError("package.json files must be an explicit list of file paths")
    expected = {"package.json"}
    for name in files:
        if (
            not isinstance(name, str)
            or not name
            or name.startswith("/")
            or any(part in {"", ".", ".."} for part in name.split("/"))
            or any(character in name for character in "\\*?[]!\x00\r\n")
            or name in expected
        ):
            raise ValueError(f"invalid or duplicate package.json files entry: {name!r}")
        expected.add(name)
    return expected


def empty_output(output: Path) -> None:
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise ValueError(f"npm release output is not an empty directory: {output}")
    else:
        output.mkdir(parents=True)


def pack(package: Path, manifest_path: Path, output: Path) -> Path:
    manifest = read_manifest(manifest_path)
    expected = manifest_files(manifest)
    if not package.is_dir():
        raise ValueError(f"npm package tree is missing: {package}")
    actual = set()
    for path in package.rglob("*"):
        # Bazel sandboxes can expose tree artifact leaves as file symlinks.
        # Read their bytes into regular tar members; never archive link metadata.
        if (path.is_symlink() and path.is_dir()) or not (path.is_file() or path.is_dir()):
            raise ValueError(f"npm package contains a directory symlink or special file: {path}")
        if path.is_file():
            actual.add(path.relative_to(package).as_posix())
    if actual != expected:
        raise ValueError(
            f"npm package files differ: missing={sorted(expected - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )
    if (package / "package.json").read_bytes() != manifest_path.read_bytes():
        raise ValueError("npm package tree package.json differs from its manifest")
    empty_output(output)
    tarball = output / tarball_name(manifest)
    directories = {"package"}
    for name in expected:
        directories.update(str(parent) for parent in PurePosixPath("package", name).parents if str(parent) != ".")
    members = {f"package/{name}": package / name for name in expected}
    # Construct headers explicitly: no host stat metadata or filesystem ordering.
    with tarball.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for name in sorted(directories | members.keys()):
                    info = tarfile.TarInfo(name)
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    if name in directories:
                        info.type = tarfile.DIRTYPE
                        info.mode = 0o755
                        archive.addfile(info)
                    else:
                        source = members[name]
                        info.mode = 0o644
                        info.size = source.stat().st_size
                        with source.open("rb") as stream:
                            archive.addfile(info, stream)
    return tarball


def assemble(packages: list[tuple[Path, Path]], output: Path, version: str) -> None:
    if not packages:
        raise ValueError("npm release must contain at least one package")
    entries = []
    names: set[str] = set()
    tarballs: set[str] = set()
    sources = []
    for directory, manifest_path in packages:
        manifest = read_manifest(manifest_path)
        name = manifest["name"]
        basename = tarball_name(manifest)
        if name in names or basename in tarballs:
            raise ValueError(f"duplicate npm release package or tarball: {name}")
        source = directory / basename
        if not source.is_file() or set(directory.iterdir()) != {source}:
            raise ValueError(f"npm release must contain exactly one tarball: {directory}")
        size = source.stat().st_size
        if size <= 0:
            raise ValueError(f"npm release tarball is empty: {basename}")
        entries.append({
            "name": name,
            "version": manifest["version"],
            "tarball": basename,
            "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "size": size,
        })
        names.add(name)
        tarballs.add(basename)
        sources.append(source)
    empty_output(output)
    for source in sources:
        shutil.copyfile(source, output / source.name)
    index = {
        "format": 1,
        "version": version,
        "package_count": len(entries),
        "packages": sorted(entries, key=lambda item: item["name"]),
    }
    (output / "npm-index.json").write_text(
        json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    # No partial SHA256SUMS: the firmware slice already owns that input basename.


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    pack_parser = commands.add_parser("pack")
    pack_parser.add_argument("--package", required=True, type=Path)
    pack_parser.add_argument("--manifest", required=True, type=Path)
    pack_parser.add_argument("--output", required=True, type=Path)
    bundle_parser = commands.add_parser("bundle")
    bundle_parser.add_argument("--package", action="append", nargs=2, type=Path, default=[])
    bundle_parser.add_argument("--output", required=True, type=Path)
    bundle_parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        if args.command == "pack":
            pack(args.package, args.manifest, args.output)
        else:
            assemble(args.package, args.output, args.version)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
