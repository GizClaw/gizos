"""Publication helpers for H2Loader Bazel artifact rules."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from projects.h2loader.tools.bazel.firmware_artifacts import (
    BundleEntry,
    data_entry_name,
    package_manifest,
    write_factory_bundle,
    write_package,
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def package_entries(source_root: Path, data_root: str, files: list[str]) -> list[BundleEntry]:
    if not files:
        return []
    root = Path(data_root)
    if root.is_absolute() or ".." in root.parts:
        raise ValueError(f"package data root must be repository-relative: {data_root}")
    entries: list[BundleEntry] = []
    names: set[str] = set()
    for value in files:
        logical_source = Path(value)
        if logical_source.is_absolute():
            raise ValueError(f"package data escapes declared root {data_root}: {value}")
        try:
            relative_path = logical_source.relative_to(root)
        except ValueError as error:
            raise ValueError(f"package data escapes declared root {data_root}: {value}") from error
        if not relative_path.parts or ".." in relative_path.parts:
            raise ValueError(f"package data escapes declared root {data_root}: {value}")
        source = source_root / logical_source
        if not source.is_file():
            raise ValueError(f"package data file is missing: {value}")
        name = data_entry_name(relative_path.as_posix())
        if name in names:
            raise ValueError(f"duplicate package data path: {name}")
        names.add(name)
        entries.append(BundleEntry(name=name, data=source.read_bytes()))
    return entries


def publish_managed_package(
    *,
    source_root: Path,
    app_image: Path,
    app_path: str,
    data_root: str,
    data_files: list[str],
    output: Path,
    board: str,
    role: str,
    target: str,
    version: str,
) -> None:
    entries = package_entries(source_root, data_root, data_files)
    write_package(
        output,
        app_path,
        app_image.read_bytes(),
        entries,
        role=role,
        board=board,
        target=target,
        version=version,
    )


def publish_esp_recovery(
    *,
    flash_root: Path,
    flash_metadata: Path,
    output: Path,
    board: str,
    target: str,
) -> None:
    metadata = json.loads(flash_metadata.read_text(encoding="utf-8"))
    flash_files = metadata.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise ValueError("ESP flash metadata has no flash_files object")
    members: list[tuple[int, str, Path]] = []
    names: set[str] = set()
    for offset, relative in sorted(flash_files.items(), key=lambda item: int(item[0], 0)):
        relative_path = Path(relative)
        if relative_path.is_absolute() or not relative_path.parts or ".." in relative_path.parts:
            raise ValueError(f"ESP flash file escapes flash root: {relative}")
        source = flash_root / relative_path
        if not source.is_file():
            raise ValueError(f"ESP flash file is missing: {relative}")
        name = source.name
        if name in names:
            raise ValueError(f"ESP recovery has duplicate basename: {name}")
        names.add(name)
        members.append((int(offset, 0), name, source))
    write_factory_bundle(
        output,
        driver=1,
        board=board,
        target=target,
        baud=115200,
        files=members,
    )


def publish_bk_recovery(
    *,
    recovery_image: Path,
    recovery_config: Path,
    output: Path,
    board: str,
    target: str,
) -> None:
    config = json.loads(recovery_config.read_text(encoding="utf-8"))
    offset = config.get("offset")
    baud = config.get("baud")
    if not isinstance(offset, str) or not isinstance(baud, int):
        raise ValueError(f"invalid BK recovery config: {recovery_config}")
    write_factory_bundle(
        output,
        driver=2,
        board=board,
        target=target,
        baud=baud,
        files=[(int(offset, 0), recovery_image.name, recovery_image)],
    )


def publish_metadata(
    *,
    output: Path,
    entry: str,
    project: str,
    app: str,
    platform: str,
    board: str,
    image: str,
    role: str,
    target: str,
    version: str,
    app_image: Path,
    package: Path,
    recovery: Path | None,
    native: list[tuple[str, Path]],
) -> None:
    def asset(path: Path, operation: str, name: str | None = None) -> dict[str, str | int]:
        asset_name = name or path.name
        normalized = Path(asset_name)
        if normalized.is_absolute() or ".." in normalized.parts:
            raise ValueError(f"invalid artifact name: {asset_name}")
        return {
            "name": normalized.as_posix(),
            "operation": operation,
            "sha256": sha256(path),
            "size": path.stat().st_size,
        }

    release_assets = [asset(package, "managed-install")]
    if recovery is not None:
        release_assets.append(asset(recovery, "recovery"))
    metadata = {
        "entry": entry,
        "project": project,
        "app": app,
        "platform": platform,
        "board": board,
        "image": image,
        "role": role,
        "target": target,
        "version": version,
        "package_manifest": package_manifest(
            app_image.read_bytes(),
            role=role,
            board=board,
            target=target,
            version=version,
        ),
        "assets": release_assets,
        "native_artifacts": [
            asset(path, "native-debug-or-flash", name)
            for name, path in native
        ],
    }
    names = [item["name"] for item in metadata["native_artifacts"]]
    if len(names) != len(set(names)):
        raise ValueError("native artifact metadata contains duplicate names")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
