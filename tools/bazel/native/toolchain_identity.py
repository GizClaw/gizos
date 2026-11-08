"""Content identity helpers for externally installed native toolchains."""

from __future__ import annotations

import argparse
import hashlib
import os
import stat
from pathlib import Path


class ToolchainIdentityError(RuntimeError):
    """The selected toolchain cannot be represented by the stable manifest."""


def directory_manifest_sha256(root: Path) -> str:
    """Hash regular-file contents, executable bits, and symlink targets below root."""
    root = root.resolve()
    if not root.is_dir():
        raise ToolchainIdentityError(f"toolchain directory is missing: {root}")

    digest = hashlib.sha256()
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        metadata = path.lstat()
        if stat.S_ISDIR(metadata.st_mode):
            continue
        if stat.S_ISLNK(metadata.st_mode):
            kind = b"symlink"
            value = os.readlink(path).encode("utf-8")
        elif stat.S_ISREG(metadata.st_mode):
            kind = b"file"
            file_digest = hashlib.sha256()
            try:
                with path.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        file_digest.update(block)
            except OSError as error:
                raise ToolchainIdentityError(
                    f"cannot hash toolchain file: {path}: {error}"
                ) from error
            value = (
                f"{stat.S_IMODE(metadata.st_mode) & 0o111:o}:"
                f"{file_digest.hexdigest()}"
            ).encode("ascii")
        else:
            raise ToolchainIdentityError(f"unsupported toolchain entry: {path}")

        for field in (kind, relative, value):
            digest.update(len(field).to_bytes(8, "big"))
            digest.update(field)
    return digest.hexdigest()


def validate_directory_manifest_sha256(
    root: Path,
    expected: str,
    label: str,
) -> None:
    try:
        actual = directory_manifest_sha256(root)
    except ToolchainIdentityError as error:
        raise ToolchainIdentityError(f"cannot identify {label}: {error}") from error
    if actual != expected:
        raise ToolchainIdentityError(
            f"{label} content SHA-256 mismatch: expected {expected}, found {actual}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", required=True)
    parser.add_argument("--expected", required=True)
    arguments = parser.parse_args()
    try:
        validate_directory_manifest_sha256(
            Path(arguments.directory),
            arguments.expected,
            "toolchain",
        )
    except ToolchainIdentityError as error:
        parser.exit(1, f"{error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
