"""Runtime helpers for versioned native dependency locator manifests."""

from __future__ import annotations

import json
import os
import shlex
import sys
from pathlib import Path


LOCATOR_SCHEMA = "h2.native-locator.v1"
FIXED_SYSTEM_PATH = ("/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin")


class NativeRuntimeError(RuntimeError):
    """A native dependency locator or fixed runtime contract is invalid."""


def read_locator(value: str, expected_kind: str) -> dict[str, object]:
    path = Path(value)
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise NativeRuntimeError(f"cannot read {expected_kind} locator: {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != LOCATOR_SCHEMA:
        raise NativeRuntimeError(f"invalid {expected_kind} locator schema: {path}")
    if document.get("kind") != expected_kind:
        raise NativeRuntimeError(
            f"native locator kind mismatch: expected {expected_kind}, "
            f"found {document.get('kind')!r}"
        )
    if document.get("enabled") is not True:
        raise NativeRuntimeError(f"{expected_kind} locator is not configured")
    paths = document.get("paths")
    if not isinstance(paths, dict):
        raise NativeRuntimeError(f"invalid {expected_kind} locator paths")
    result: dict[str, object] = dict(document)
    normalized: dict[str, str] = {}
    for name, raw_path in paths.items():
        if not isinstance(name, str) or not isinstance(raw_path, str):
            raise NativeRuntimeError(f"invalid {expected_kind} locator path entry")
        candidate = Path(raw_path)
        if not candidate.is_absolute():
            raise NativeRuntimeError(
                f"{expected_kind} locator path must be absolute: {name}={raw_path}"
            )
        normalized[name] = str(candidate.resolve())
    result["paths"] = normalized
    return result


def locator_path(document: dict[str, object], name: str, kind: str) -> Path:
    paths = document["paths"]
    assert isinstance(paths, dict)
    value = paths.get(name)
    if not isinstance(value, str):
        raise NativeRuntimeError(f"{kind} locator has no {name} path")
    return Path(value)


def fixed_environment(tool_directories: list[Path] | None = None) -> dict[str, str]:
    directories: list[str] = []
    for directory in [*(tool_directories or []), *(Path(value) for value in FIXED_SYSTEM_PATH)]:
        value = str(directory)
        if value not in directories and directory.is_dir():
            directories.append(value)
    return {
        "HOME": "/tmp",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "PATH": os.pathsep.join(directories),
        "TZ": "UTC",
    }


def find_unique_executable(root: Path, name: str, label: str) -> Path:
    matches = sorted(
        path.resolve()
        for path in root.rglob(name)
        if path.is_file() and os.access(path, os.X_OK)
    )
    if len(matches) != 1:
        raise NativeRuntimeError(
            f"expected exactly one {label} under {root}, found {len(matches)}"
        )
    return matches[0]


def find_external_repository_root(
    paths: list[Path], repository_name: str
) -> Path | None:
    """Find one Bazel external repository root containing declared inputs."""
    suffix = "+" + repository_name
    matches: set[Path] = set()
    for path in paths:
        for candidate in (path, *path.parents):
            if candidate.name == repository_name or candidate.name.endswith(suffix):
                matches.add(candidate.resolve())
                break
    if len(matches) > 1:
        rendered = ", ".join(str(path) for path in sorted(matches))
        raise NativeRuntimeError(
            f"multiple Bazel repositories matched {repository_name}: {rendered}"
        )
    return next(iter(matches), None)


def create_python_wrappers(directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=False)
    executable = Path(os.path.realpath(sys.executable))
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise NativeRuntimeError(f"Bazel Python runtime is unavailable: {executable}")
    for name in ("python", "python3", "python3.11"):
        wrapper = directory / name
        wrapper.write_text(
            "#!/bin/sh\nexec " + shlex.quote(str(executable)) + ' "$@"\n',
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
    return directory
