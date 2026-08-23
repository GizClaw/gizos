#!/usr/bin/env python3
"""Build and assemble one closed cross-runner release slice."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.bazel import cache_options  # noqa: E402


VERSION_PATTERN = re.compile(r"^[0-9]+(?:\.[0-9]+){0,2}$")
SLICES = (
    "catalog",
    "esp32s3",
    "esp32p4",
    "bk7258",
    "firmware-bundle",
    "release-bundle",
)
PRODUCERS = frozenset({"catalog"})
FIRMWARE_SLICES = {
    "esp32s3": ("esp", "esp32s3"),
    "esp32p4": ("esp", "esp32p4"),
    "bk7258": ("bk7258", "bk7258"),
}


class ReleaseError(RuntimeError):
    """A release slice cannot be built or trusted."""


def command(
    root: Path,
    args: list[str],
    *,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        args,
        cwd=root,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ReleaseError(
            f"{' '.join(args[:2])} failed with exit "
            f"{result.returncode}: {detail}"
        )
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_version(version: str) -> None:
    if not VERSION_PATTERN.fullmatch(version):
        raise ReleaseError(
            "RELEASE_VERSION must contain one to three numeric components"
        )


def resolve_output(root: Path, slice_name: str, value: Path | None) -> Path:
    output = value or Path("build/release") / slice_name
    if output.is_absolute():
        if os.environ.get("GITHUB_ACTIONS") != "true":
            raise ReleaseError(
                "absolute release staging is allowed only in GitHub Actions"
            )
        resolved = output.resolve(strict=False)
    else:
        resolved = (root / output).resolve(strict=False)
        build_root = (root / "build").resolve(strict=False)
        if not resolved.is_relative_to(build_root):
            raise ReleaseError(
                f"release staging must stay below build/: {output}"
            )
    if output.is_symlink():
        raise ReleaseError(f"release staging must not be a symlink: {output}")
    return resolved


def prepare_output(path: Path) -> None:
    if path.exists():
        if path.is_symlink() or not path.is_dir():
            raise ReleaseError(f"invalid release staging directory: {path}")
        shutil.rmtree(path)
    path.mkdir(parents=True)


def input_files(path: Path | None) -> list[Path]:
    if path is None:
        raise ReleaseError("RELEASE_INPUT_DIR is required for this slice")
    if path.is_symlink() or not path.is_dir():
        raise ReleaseError(f"invalid release input directory: {path}")
    files: list[Path] = []
    names: set[str] = set()
    for candidate in sorted(path.rglob("*")):
        if candidate.is_symlink():
            raise ReleaseError(f"release input must not be a symlink: {candidate}")
        if not candidate.is_file():
            continue
        if candidate.name in names:
            raise ReleaseError(
                f"duplicate release input basename: {candidate.name}"
            )
        names.add(candidate.name)
        files.append(candidate)
    if not files:
        raise ReleaseError("release input directory is empty")
    return files


def load_catalog(files: list[Path], version: str) -> list[dict[str, str]]:
    matches = [path for path in files if path.name == "firmware-catalog.json"]
    if len(matches) != 1:
        raise ReleaseError("release input must contain one firmware-catalog.json")
    try:
        catalog = json.loads(matches[0].read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReleaseError(f"invalid firmware catalog: {error}") from error
    if not isinstance(catalog, list) or not catalog:
        raise ReleaseError("firmware catalog must be a non-empty array")
    entries: set[str] = set()
    for item in catalog:
        if (
            not isinstance(item, dict)
            or item.get("platform") not in {"esp", "bk7258"}
            or not isinstance(item.get("entry"), str)
            or not isinstance(item.get("label"), str)
            or not item["label"].endswith(":package")
            or not isinstance(item.get("target"), str)
            or item.get("version") != version
        ):
            raise ReleaseError("firmware catalog contains an invalid entry")
        entry = item["entry"]
        if entry in entries:
            raise ReleaseError(f"duplicate firmware catalog entry: {entry}")
        entries.add(entry)
    return catalog


def copy_unique(files: list[Path], output: Path) -> None:
    names: set[str] = set()
    for source in files:
        if source.name in names or (output / source.name).exists():
            raise ReleaseError(f"duplicate staged release file: {source.name}")
        names.add(source.name)
        shutil.copyfile(source, output / source.name)


def build_catalog(root: Path, bazel: str, version: str, output: Path) -> None:
    result = command(
        root,
        [
            bazel,
            "cquery",
            "--config=ci-graph",
            f"--//tools/bazel:firmware_version={version}",
            'kind("h2loader_tar_zlib rule", //projects/h2loader/...)',
            "--output=starlark",
            "--starlark:file=tools/bazel/firmware_catalog.cquery",
        ],
    )
    if "ERROR:" in result.stderr:
        raise ReleaseError(f"Bazel firmware catalog reported an error: {result.stderr.strip()}")
    try:
        catalog = [
            json.loads(line)
            for line in result.stdout.splitlines()
            if line.strip()
        ]
    except json.JSONDecodeError as error:
        raise ReleaseError(f"invalid Bazel firmware catalog: {error}") from error
    catalog.sort(key=lambda item: item.get("entry", ""))
    path = output / "firmware-catalog.json"
    path.write_text(
        json.dumps(catalog, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    load_catalog([path], version)


def build_firmware(
    root: Path,
    bazel: str,
    slice_name: str,
    version: str,
    files: list[Path],
    output: Path,
) -> None:
    catalog = load_catalog(files, version)
    platform, target = FIRMWARE_SLICES[slice_name]
    selected = [
        item
        for item in catalog
        if item["platform"] == platform
        and (platform != "esp" or item["target"] == target)
    ]
    if not selected:
        raise ReleaseError(f"release catalog has no entries for {slice_name}")
    labels = sorted(item["label"] for item in selected)
    command(
        root,
        [
            bazel,
            "build",
            *cache_options(),
            f"--config={target}",
            f"--//tools/bazel:firmware_version={version}",
            "--output_groups=release",
            *labels,
        ],
    )
    assets: list[Path] = []
    for label in labels:
        result = command(
            root,
            [
                bazel,
                "cquery",
                f"--config={target}",
                f"--//tools/bazel:firmware_version={version}",
                label,
                "--output=starlark",
                "--starlark:file=tools/bazel/firmware_release_files.cquery",
            ],
        )
        paths = [Path(line) for line in result.stdout.splitlines() if line]
        if not paths or any(not path.is_file() for path in paths):
            raise ReleaseError(f"missing declared release outputs for {label}")
        assets.extend(paths)
    copy_unique(assets, output)


def build_firmware_bundle(
    root: Path,
    bazel: str,
    version: str,
    input_dir: Path,
    files: list[Path],
    output: Path,
) -> None:
    load_catalog(files, version)
    environment = dict(os.environ)
    environment["H2_FIRMWARE_RELEASE_INPUT_DIR"] = str(input_dir.resolve())
    options = [
        "--repo_env=H2_FIRMWARE_RELEASE_INPUT_DIR",
        f"--//tools/bazel:firmware_version={version}",
    ]
    label = "//tools/bazel:firmware_release_bundle"
    command(
        root,
        [bazel, "build", *cache_options(), *options, label],
        env=environment,
    )
    result = command(
        root,
        [bazel, "cquery", *options, "--output=files", label],
        env=environment,
    )
    paths = [Path(line) for line in result.stdout.splitlines() if line]
    if len(paths) != 1 or not paths[0].is_dir():
        raise ReleaseError("firmware bundle target returned an invalid output")
    copy_unique(
        sorted(path for path in paths[0].iterdir() if path.is_file()),
        output,
    )


def checksum_entries(path: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/]+)", line)
        if not match or match.group(2) in entries:
            raise ReleaseError(f"invalid checksum file: {path.name}")
        entries[match.group(2)] = match.group(1)
    if not entries:
        raise ReleaseError(f"checksum file is empty: {path.name}")
    return entries


def validate_checksums(
    checksums: Path,
    files: dict[str, Path],
    expected: set[str],
) -> None:
    entries = checksum_entries(checksums)
    if set(entries) != expected:
        raise ReleaseError(
            f"checksum coverage differs: expected={sorted(expected)}, "
            f"found={sorted(entries)}"
        )
    for name, digest in entries.items():
        if sha256(files[name]) != digest:
            raise ReleaseError(f"checksum mismatch: {name}")


def assemble_final(
    files: list[Path],
    output: Path,
    version: str,
) -> None:
    by_name = {path.name: path for path in files}
    names = set(by_name)
    required = {"firmware-index.json", "SHA256SUMS"}
    if missing := required - names:
        raise ReleaseError(f"final release input is incomplete: {sorted(missing)}")
    try:
        index = json.loads(
            by_name["firmware-index.json"].read_text(encoding="utf-8")
        )
    except json.JSONDecodeError as error:
        raise ReleaseError(f"invalid firmware index: {error}") from error
    if not isinstance(index, dict):
        raise ReleaseError("firmware index must be an object")
    firmware = index.get("firmware")
    if (
        index.get("format") != 1
        or index.get("version") != version
        or not isinstance(firmware, list)
        or index.get("firmware_count") != len(firmware)
        or not firmware
    ):
        raise ReleaseError("firmware index identity is invalid")
    asset_names: set[str] = set()
    for item in firmware:
        if (
            not isinstance(item, dict)
            or item.get("platform") not in {"esp", "bk7258"}
            or item.get("version") != version
            or not isinstance(item.get("assets"), list)
        ):
            raise ReleaseError("firmware index contains an invalid entry")
        for asset in item["assets"]:
            name = asset.get("name") if isinstance(asset, dict) else None
            if (
                not isinstance(name, str)
                or Path(name).name != name
                or name in asset_names
            ):
                raise ReleaseError(f"invalid firmware release asset: {name}")
            asset_names.add(name)
    expected = {
        "firmware-index.json",
        "SHA256SUMS",
        *asset_names,
    }
    if names != expected:
        raise ReleaseError(
            f"final release inputs differ: missing={sorted(expected - names)}, "
            f"unexpected={sorted(names - expected)}"
        )
    validate_checksums(
        by_name["SHA256SUMS"],
        by_name,
        {"firmware-index.json", *asset_names},
    )
    copy_unique(
        [path for path in files if path.name != "SHA256SUMS"],
        output,
    )
    assets = sorted(path for path in output.iterdir() if path.is_file())
    (output / "SHA256SUMS").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in assets),
        encoding="ascii",
    )


def run_slice(
    root: Path,
    bazel: str,
    slice_name: str,
    version: str,
    input_dir: Path | None,
    output: Path,
) -> None:
    validate_version(version)
    if slice_name not in SLICES:
        raise ReleaseError(f"unknown RELEASE_SLICE: {slice_name}")
    if slice_name in PRODUCERS and input_dir is not None:
        raise ReleaseError(f"{slice_name} does not accept RELEASE_INPUT_DIR")
    files = [] if slice_name in PRODUCERS else input_files(input_dir)
    prepare_output(output)
    if slice_name == "catalog":
        build_catalog(root, bazel, version, output)
    elif slice_name in FIRMWARE_SLICES:
        build_firmware(root, bazel, slice_name, version, files, output)
    elif slice_name == "firmware-bundle":
        assert input_dir is not None
        build_firmware_bundle(
            root, bazel, version, input_dir, files, output
        )
    else:
        assemble_final(files, output, version)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--slice", default=os.environ.get("RELEASE_SLICE"))
    parser.add_argument("--version", default=os.environ.get("RELEASE_VERSION"))
    parser.add_argument("--input", default=os.environ.get("RELEASE_INPUT_DIR"))
    parser.add_argument("--output", default=os.environ.get("RELEASE_STAGING_DIR"))
    parser.add_argument("--bazel", default=os.environ.get("BAZEL_BIN", "bazel"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    try:
        if not args.slice:
            raise ReleaseError("RELEASE_SLICE is required")
        if args.slice not in SLICES:
            raise ReleaseError(f"unsupported RELEASE_SLICE: {args.slice}")
        if not args.version:
            raise ReleaseError("RELEASE_VERSION is required")
        input_dir = Path(args.input) if args.input else None
        output_value = Path(args.output) if args.output else None
        output = resolve_output(root, args.slice, output_value)
        run_slice(
            root,
            args.bazel,
            args.slice,
            args.version,
            input_dir,
            output,
        )
    except (OSError, ReleaseError, ValueError, subprocess.SubprocessError) as error:
        print(f"error: release slice failed: {error}", file=sys.stderr)
        return 1
    print(f"release slice complete: {args.slice} -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
