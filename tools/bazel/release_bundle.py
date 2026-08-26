#!/usr/bin/env python3
"""Validate and assemble Bazel firmware outputs without inferring identity."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys


FIRMWARE_VERSION_PATTERN = re.compile(
    r"^(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)"
    r"(?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


def validate_firmware_version(value: object, entry: object) -> str:
    if (
        not isinstance(value, str)
        or len(value.encode("ascii", errors="ignore")) != len(value)
        or len(value) > 31
        or not FIRMWARE_VERSION_PATTERN.fullmatch(value)
    ):
        raise ValueError(f"invalid firmware version: {entry}")
    return value


def validate_package_manifest(item: dict[str, object]) -> None:
    manifest = item.get("package_manifest")
    if not isinstance(manifest, dict) or manifest.get("format") != 1:
        raise ValueError(f"invalid package manifest: {item.get('entry')}")
    for key in ("board", "role", "target", "version"):
        if manifest.get(key) != item.get(key):
            raise ValueError(f"package manifest {key} mismatch: {item.get('entry')}")
    image_size = manifest.get("image_size")
    image_sha256 = manifest.get("image_sha256")
    if (
        type(image_size) is not int
        or image_size <= 0
        or not isinstance(image_sha256, str)
        or len(image_sha256) != 64
        or any(character not in "0123456789abcdef" for character in image_sha256)
    ):
        raise ValueError(f"invalid package image metadata: {item.get('entry')}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def assemble(inputs: list[Path], output: Path, version: str) -> None:
    by_name: dict[str, Path] = {}
    for path in inputs:
        if path.name in by_name:
            raise ValueError(f"duplicate release input basename: {path.name}")
        by_name[path.name] = path
    catalog_path = by_name.get("firmware-catalog.json")
    if catalog_path is None:
        raise ValueError("firmware-catalog.json is missing")
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    if not isinstance(catalog, list) or not catalog:
        raise ValueError("firmware catalog must be a non-empty array")
    required_identity = ("entry", "platform", "board", "image", "role", "target")
    for item in catalog:
        if not isinstance(item, dict) or any(
            not isinstance(item.get(key), str) or not item[key]
            for key in required_identity
        ):
            raise ValueError("firmware catalog contains an invalid identity")
        validate_firmware_version(item.get("version"), item["entry"])
    expected = {item["entry"] for item in catalog}
    if len(expected) != len(catalog):
        raise ValueError("firmware catalog contains duplicate entries")

    metadata_paths = sorted(path for path in inputs if path.name.endswith(".firmware.json"))
    firmware = [json.loads(path.read_text(encoding="utf-8")) for path in metadata_paths]
    if any(not isinstance(item, dict) for item in firmware):
        raise ValueError("firmware metadata must be a JSON object")
    actual = {item.get("entry") for item in firmware}
    if len(actual) != len(firmware) or actual != expected:
        raise ValueError(
            f"firmware coverage mismatch: missing={sorted(expected - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )
    catalog_identity = {
        item["entry"]: {key: item[key] for key in ("platform", "board", "image", "role", "target", "version")}
        for item in catalog
    }
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise ValueError(f"release output is not an empty directory: {output}")
    else:
        output.mkdir(parents=True)
    asset_names: set[str] = set()
    for item in firmware:
        validate_firmware_version(item.get("version"), item.get("entry"))
        identity = {key: item.get(key) for key in ("platform", "board", "image", "role", "target", "version")}
        if identity != catalog_identity[item["entry"]]:
            raise ValueError(f"firmware identity mismatch: {item['entry']}")
        validate_package_manifest(item)
        assets = item.get("assets")
        if not isinstance(assets, list) or not assets:
            raise ValueError(f"firmware has no release assets: {item['entry']}")
        for asset in assets:
            name = asset.get("name") if isinstance(asset, dict) else None
            if not isinstance(name, str) or Path(name).name != name or name in asset_names:
                raise ValueError(f"invalid or duplicate release asset: {name}")
            source = by_name.get(name)
            if source is None:
                raise ValueError(f"release asset is missing: {name}")
            if asset.get("size") != source.stat().st_size or asset.get("sha256") != sha256(source):
                raise ValueError(f"release asset integrity mismatch: {name}")
            asset_names.add(name)
            shutil.copyfile(source, output / name)

    consumed_names = {
        "firmware-catalog.json",
        *(path.name for path in metadata_paths),
        *asset_names,
    }
    for name, source in by_name.items():
        if name in consumed_names:
            continue
        if not (name.endswith(".zip") or name.endswith(".sha256")):
            raise ValueError(f"unexpected release input: {name}")
        shutil.copyfile(source, output / name)

    index = {
        "format": 1,
        "version": version,
        "firmware_count": len(firmware),
        "firmware": sorted(firmware, key=lambda item: item["entry"]),
    }
    index_path = output / "firmware-index.json"
    index_path.write_text(json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    assets = sorted(path for path in output.iterdir() if path.is_file())
    (output / "SHA256SUMS").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in assets),
        encoding="ascii",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        assemble(args.input, args.output, args.version)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
