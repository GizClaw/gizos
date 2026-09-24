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


RELEASE_NAME_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]{0,190}$")
LOADER_ROOT = "projects/h2loader/targets/h2loader_tar_zlib/loader/"
PLATFORM_TARGETS = {"esp": {"esp32s3", "esp32p4"}, "bk7258": {"bk7258"}, "jieli": {"wl82"}}


def release_name_from_identity(item: dict[str, object]) -> str:
    for key in ("image", "board"):
        if not isinstance(item.get(key), str) or not RELEASE_NAME_PATTERN.fullmatch(item[key]):
            raise ValueError(f"invalid firmware {key}")
    name = f"{item['image']}-{item['board']}"
    if not RELEASE_NAME_PATTERN.fullmatch(name):
        raise ValueError("invalid firmware release name")
    return name


def validate_catalog(catalog: object) -> list[dict[str, object]]:
    if not isinstance(catalog, list) or not catalog:
        raise ValueError("firmware catalog must be a non-empty array")
    entries: set[str] = set()
    names: set[str] = set()
    for item in catalog:
        if not isinstance(item, dict) or any(
            not isinstance(item.get(key), str) or not item[key]
            for key in ("entry", "label", "platform", "board", "image", "role", "target")
        ):
            raise ValueError("firmware catalog contains an invalid entry")
        validate_firmware_version(item.get("version"), item["entry"])
        if item["target"] not in PLATFORM_TARGETS.get(item["platform"], set()):
            raise ValueError("firmware catalog contains an invalid entry platform/target")
        # Only canonical Loader entries may opt in. Alternate packages and all
        # e2e/example launchers remain diagnostic even if accidentally tagged.
        if (
            "/e2e/" in f"/{item['entry']}/"
            or item["entry"] != LOADER_ROOT + item["board"]
            or item["label"] != "//" + item["entry"] + ":package"
            or item["image"] != "loader"
            or item["role"] != "h2loader"
        ):
            raise ValueError("diagnostic or invalid entry in firmware catalog")
        name = release_name_from_identity(item)
        if item.get("release_name") != name:
            raise ValueError("firmware release_name does not match image and board")
        if item["entry"] in entries or name in names:
            raise ValueError("duplicate firmware catalog entry or release_name")
        entries.add(item["entry"])
        names.add(name)
    return catalog


def expected_asset_contracts(item: dict[str, object]) -> set[tuple[str, str]]:
    contracts = {(".update.tar.zlib", "managed-install")}
    if item["platform"] in {"esp", "bk7258"}:
        contracts.add((".recovery.h2fb", "recovery"))
    if item["platform"] == "esp":
        contracts.add((".combined_factory.bin", "factory-flash"))
    return contracts


def validate_assets(item: dict[str, object], *, canonical: bool) -> list[dict[str, object]]:
    assets = item.get("assets")
    if not isinstance(assets, list) or not assets:
        raise ValueError("firmware has no release assets")
    contracts = set()
    names = set()
    for asset in assets:
        if not isinstance(asset, dict):
            raise ValueError("invalid firmware release asset")
        name = asset.get("name")
        if (not isinstance(name, str) or Path(name).name != name or "\\" in name
                or name in names):
            raise ValueError(f"invalid or duplicate release asset: {name}")
        contract = (asset.get("release_suffix"), asset.get("operation"))
        if (not all(isinstance(value, str) for value in contract)
                or contract not in expected_asset_contracts(item) or contract in contracts):
            raise ValueError(f"invalid or duplicate firmware asset contract: {name}")
        if canonical and name != item["release_name"] + asset["release_suffix"]:
            raise ValueError(f"noncanonical firmware release asset: {name}")
        if asset["operation"] == "factory-flash":
            if type(asset.get("flash_offset")) is not int or asset["flash_offset"] != 0:
                raise ValueError(f"invalid factory flash offset: {name}")
        elif "flash_offset" in asset:
            raise ValueError(f"unexpected firmware flash offset: {name}")
        if (type(asset.get("size")) is not int or asset["size"] <= 0
                or not isinstance(asset.get("sha256"), str)
                or re.fullmatch(r"[0-9a-f]{64}", asset["sha256"]) is None):
            raise ValueError(f"invalid firmware asset integrity: {name}")
        names.add(name)
        contracts.add(contract)
    if contracts != expected_asset_contracts(item):
        raise ValueError("firmware asset set mismatch")
    return assets


def validate_index(index: object, batch: str) -> dict[str, dict[str, object]]:
    if (not isinstance(index, dict) or type(index.get("format")) is not int
            or index["format"] != 1 or index.get("batch") != batch
            or not isinstance(index.get("firmware"), list)
            or type(index.get("firmware_count")) is not int
            or index["firmware_count"] != len(index["firmware"])):
        raise ValueError("firmware index identity is invalid")
    firmware = index["firmware"]
    validate_catalog([{**item, "label": "//" + str(item.get("entry")) + ":package"}
                      if isinstance(item, dict) else item for item in firmware])
    assets = {}
    for item in firmware:
        validate_package_manifest(item)
        for asset in validate_assets(item, canonical=True):
            if asset["name"] in assets:
                raise ValueError("duplicate firmware release asset")
            assets[asset["name"]] = asset
    return assets


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def assemble(inputs: list[Path], output: Path, batch: str) -> None:
    by_name: dict[str, Path] = {}
    for path in inputs:
        if path.name in by_name:
            raise ValueError(f"duplicate release input basename: {path.name}")
        by_name[path.name] = path
    catalog_path = by_name.get("firmware-catalog.json")
    if catalog_path is None:
        raise ValueError("firmware-catalog.json is missing")
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    validate_catalog(catalog)
    expected = {item["entry"] for item in catalog}
    catalog_by_entry = {item["entry"]: item for item in catalog}

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
    published_names: set[str] = set()
    for item in firmware:
        validate_firmware_version(item.get("version"), item.get("entry"))
        identity = {key: item.get(key) for key in ("platform", "board", "image", "role", "target", "version")}
        if identity != catalog_identity[item["entry"]]:
            raise ValueError(f"firmware identity mismatch: {item['entry']}")
        validate_package_manifest(item)
        item["release_name"] = catalog_by_entry[item["entry"]]["release_name"]
        assets = validate_assets(item, canonical=False)
        rewritten = []
        for asset in assets:
            name = asset["name"]
            if name in asset_names:
                raise ValueError(f"duplicate release asset: {name}")
            source = by_name.get(name)
            if source is None:
                raise ValueError(f"release asset is missing: {name}")
            if asset["size"] != source.stat().st_size or asset["sha256"] != sha256(source):
                raise ValueError(f"release asset integrity mismatch: {name}")
            published = item["release_name"] + asset["release_suffix"]
            if published in published_names:
                raise ValueError(f"duplicate published release asset: {published}")
            published_names.add(published)
            asset_names.add(name)
            shutil.copyfile(source, output / published)
            rewritten.append({**asset, "name": published})
        item["assets"] = rewritten

    consumed_names = {
        "firmware-catalog.json",
        *(path.name for path in metadata_paths),
        *asset_names,
    }
    if set(by_name) != consumed_names:
        raise ValueError(f"unexpected release input: {sorted(set(by_name) - consumed_names)}")

    index = {
        "format": 1,
        "batch": batch,
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
    parser.add_argument("--batch", required=True)
    args = parser.parse_args()
    try:
        assemble(args.input, args.output, args.batch)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
