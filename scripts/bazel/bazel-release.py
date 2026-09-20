#!/usr/bin/env python3
"""Build and assemble one closed cross-runner release slice."""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.bazel import cache_options  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.bazel.release_bundle import (  # noqa: E402
    validate_catalog, validate_index,
)


BATCH_PATTERN = re.compile(r"^[0-9]{8}-[0-9]{6}$")
RELEASE_TAG = "firmware-release"
RELEASE_QUERY = (
    'attr("tags", "firmware-release", '
    'kind("h2loader_tar_zlib rule", //projects/...))'
)
SLICES = (
    "catalog",
    "npm-packages",
    "esp32s3",
    "esp32p4",
    "bk7258",
    "ac791n",
    "firmware-bundle",
    "package",
    "release-bundle",
)
PRODUCERS = frozenset({"catalog", "npm-packages"})
CATALOG_CONFIGS = ("esp32s3", "esp32p4", "bk7258", "ac791n")
FIRMWARE_SLICES = {
    "esp32s3": ("esp", "esp32s3"),
    "esp32p4": ("esp", "esp32p4"),
    "bk7258": ("bk7258", "bk7258"),
    "ac791n": ("jieli", "wl82"),
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


def validate_batch(batch: str) -> None:
    if not BATCH_PATTERN.fullmatch(batch):
        raise ReleaseError("RELEASE_BATCH must use UTC YYYYMMDD-HHMMSS format")
    try:
        timestamp = datetime.strptime(batch, "%Y%m%d-%H%M%S")
    except ValueError as error:
        raise ReleaseError("RELEASE_BATCH contains an invalid UTC timestamp") from error
    if not 1980 <= timestamp.year <= 2107:
        raise ReleaseError("RELEASE_BATCH must fit the ZIP timestamp range 1980..2107")


def zip_timestamp(batch: str) -> tuple[int, ...]:
    """Return the batch timestamp floored to DOS's two-second resolution."""
    validate_batch(batch)
    timestamp = datetime.strptime(batch, "%Y%m%d-%H%M%S")
    return timestamp.replace(second=timestamp.second // 2 * 2).timetuple()[:6]


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


def load_catalog(files: list[Path]) -> list[dict[str, str]]:
    matches = [path for path in files if path.name == "firmware-catalog.json"]
    if len(matches) != 1:
        raise ReleaseError("release input must contain one firmware-catalog.json")
    try:
        catalog = json.loads(matches[0].read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReleaseError(f"invalid firmware catalog: {error}") from error
    try:
        return validate_catalog(catalog)
    except ValueError as error:
        raise ReleaseError(str(error)) from error


def copy_unique(files: list[Path], output: Path) -> None:
    names: set[str] = set()
    for source in files:
        if source.name in names or (output / source.name).exists():
            raise ReleaseError(f"duplicate staged release file: {source.name}")
        names.add(source.name)
        shutil.copyfile(source, output / source.name)


def build_catalog(root: Path, bazel: str, output: Path) -> None:
    discovery = command(root, [bazel, "query", RELEASE_QUERY, "--output=xml"])
    try:
        query = ET.fromstring(discovery.stdout)
    except ET.ParseError as error:
        raise ReleaseError(f"invalid Bazel query XML: {error}") from error
    labels = []
    for rule in query.findall("rule"):
        label = rule.get("name")
        tags = [value.get("value") for values in rule.findall("list")
                if values.get("name") == "tags" for value in values.findall("string")]
        if not label or tags.count(RELEASE_TAG) != 1:
            raise ReleaseError(f"firmware target must declare exactly one {RELEASE_TAG} tag: {label}")
        labels.append(label)
    labels.sort()
    if not labels:
        raise ReleaseError("Bazel firmware package discovery returned no labels")
    expression = "set(%s)" % " ".join(labels)
    catalog: list[dict[str, str]] = []
    for config in CATALOG_CONFIGS:
        result = command(
            root,
            [
                bazel,
                "cquery",
                f"--config={config}",
                expression,
                "--output=starlark",
                "--starlark:file=tools/bazel/firmware_catalog.cquery",
            ],
        )
        if "ERROR:" in result.stderr:
            raise ReleaseError(
                "Bazel firmware catalog reported an error for "
                f"{config}: {result.stderr.strip()}"
            )
        try:
            catalog.extend(
                json.loads(line)
                for line in result.stdout.splitlines()
                if line.strip()
            )
        except json.JSONDecodeError as error:
            raise ReleaseError(
                f"invalid Bazel firmware catalog for {config}: {error}"
            ) from error
    try:
        validate_catalog(catalog)
    except ValueError as error:
        raise ReleaseError(str(error)) from error
    if {item["label"] for item in catalog} != set(labels):
        raise ReleaseError("firmware catalog coverage differs from opted-in targets")
    catalog.sort(key=lambda item: item["entry"])
    (output / "firmware-catalog.json").write_text(
        json.dumps(catalog, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def build_firmware(
    root: Path,
    bazel: str,
    slice_name: str,
    files: list[Path],
    output: Path,
) -> None:
    catalog = load_catalog(files)
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
    credentials_rc = None
    credentials = os.environ.get("H2LOADER_WIFI_CREDENTIALS", "")
    if target == "esp32s3" and credentials:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            prefix="h2-release-action-env-",
            suffix=".bazelrc",
            delete=False,
        ) as stream:
            if hasattr(os, "fchmod"):
                os.fchmod(stream.fileno(), 0o600)
            stream.write(
                "build:esp --action_env=H2LOADER_WIFI_CREDENTIALS="
                + shlex.quote(credentials)
                + "\n"
            )
            credentials_rc = Path(stream.name)
    startup_options = (
        [f"--bazelrc={credentials_rc}"] if credentials_rc is not None else []
    )
    build_options = ["--noannounce_rc"] if credentials_rc is not None else []
    try:
        command(
            root,
            [
                bazel,
                *startup_options,
                "build",
                *build_options,
                *cache_options(),
                f"--config={slice_name}",
                "--output_groups=release",
                *labels,
            ],
        )
    finally:
        if credentials_rc is not None:
            credentials_rc.unlink(missing_ok=True)
    assets: list[Path] = []
    for label in labels:
        result = command(
            root,
            [
                bazel,
                "cquery",
                f"--config={slice_name}",
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
    batch: str,
    input_dir: Path,
    files: list[Path],
    output: Path,
) -> None:
    load_catalog(files)
    environment = dict(os.environ)
    environment["H2_FIRMWARE_RELEASE_INPUT_DIR"] = str(input_dir.resolve())
    options = [
        "--repo_env=H2_FIRMWARE_RELEASE_INPUT_DIR",
        f"--//tools/bazel:release_batch={batch}",
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


def build_npm_packages(
    root: Path,
    bazel: str,
    batch: str,
    output: Path,
) -> None:
    label = "//tools/bazel:npm_release_bundle"
    options = [f"--//tools/bazel:release_batch={batch}"]
    command(root, [bazel, "build", *cache_options(), *options, label])
    result = command(root, [bazel, "cquery", *options, "--output=files", label])
    paths = [Path(line) for line in result.stdout.splitlines() if line]
    if len(paths) != 1 or not paths[0].is_dir():
        raise ReleaseError("npm bundle target returned an invalid output")
    copy_unique(input_files(paths[0]), output)


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


def bundle_assets(index: object, batch: str) -> dict[str, dict[str, object]]:
    try:
        return validate_index(index, batch)
    except ValueError as error:
        raise ReleaseError(str(error)) from error


def package_bundle(files: list[Path], output: Path, batch: str) -> None:
    timestamp = zip_timestamp(batch)
    by_name = {path.name: path for path in files}
    if len(by_name) != len(files):
        raise ReleaseError("release bundle contains duplicate basenames")
    if not {"firmware-index.json", "SHA256SUMS"} <= set(by_name):
        raise ReleaseError("release bundle must contain index and checksums")
    assets = bundle_assets(json.loads(by_name["firmware-index.json"].read_text()), batch)
    expected = set(assets) | {"firmware-index.json", "SHA256SUMS"}
    if set(by_name) != expected:
        raise ReleaseError("release bundle contains unexpected or missing files")
    validate_checksums(by_name["SHA256SUMS"], by_name, expected - {"SHA256SUMS"})
    for name, asset in assets.items():
        if by_name[name].stat().st_size != asset["size"] or sha256(by_name[name]) != asset["sha256"]:
            raise ReleaseError(f"firmware asset integrity mismatch: {name}")
    stem = f"firmware-release-v{batch}"
    with zipfile.ZipFile(output / f"{stem}.zip", "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(by_name):
            info = zipfile.ZipInfo(f"{stem}/{name}", date_time=timestamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, by_name[name].read_bytes(), compresslevel=9)


def validate_archive(path: Path, batch: str) -> None:
    timestamp = zip_timestamp(batch)
    prefix = f"firmware-release-v{batch}/"
    try:
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            by_name = {}
            for info in members:
                name = info.filename.removeprefix(prefix)
                if (info.filename != prefix + name or "/" in name or "\\" in name
                        or name in {"", ".", ".."} or name in by_name
                        or info.create_system != 3 or info.external_attr != 0o100644 << 16
                        or info.compress_type != zipfile.ZIP_DEFLATED):
                    raise ReleaseError(f"invalid firmware ZIP member: {info.filename}")
                if info.date_time != timestamp:
                    raise ReleaseError(
                        f"firmware ZIP member timestamp mismatch: {info.filename}: "
                        f"expected {timestamp} for batch {batch}, got {info.date_time}"
                    )
                by_name[name] = info
            if not {"firmware-index.json", "SHA256SUMS"} <= set(by_name):
                raise ReleaseError("firmware ZIP must contain index and checksums")
            index = json.loads(archive.read(by_name["firmware-index.json"]))
            assets = bundle_assets(index, batch)
            expected = set(assets) | {"firmware-index.json"}
            if set(by_name) != expected | {"SHA256SUMS"}:
                raise ReleaseError("firmware ZIP contains unexpected or missing files")
            checksums = {}
            for line in archive.read(by_name["SHA256SUMS"]).decode("ascii").splitlines():
                match = re.fullmatch(r"([0-9a-f]{64})  ([^/]+)", line)
                if match is None or match[2] in checksums:
                    raise ReleaseError("firmware ZIP contains invalid checksums")
                checksums[match[2]] = match[1]
            if set(checksums) != expected:
                raise ReleaseError("firmware ZIP checksum coverage differs")
            for name in sorted(expected):
                digest = hashlib.sha256()
                with archive.open(by_name[name]) as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        digest.update(block)
                if digest.hexdigest() != checksums[name]:
                    raise ReleaseError(f"firmware ZIP checksum mismatch: {name}")
                if name in assets and (digest.hexdigest() != assets[name]["sha256"]
                                       or by_name[name].file_size != assets[name]["size"]):
                    raise ReleaseError(f"firmware ZIP asset integrity mismatch: {name}")
    except (zipfile.BadZipFile, UnicodeError, json.JSONDecodeError, RuntimeError) as error:
        raise ReleaseError(f"invalid firmware ZIP: {error}") from error


def assemble_final(
    files: list[Path],
    output: Path,
    batch: str,
) -> None:
    validate_batch(batch)
    by_name = {path.name: path for path in files}
    if len(by_name) != len(files):
        raise ReleaseError("final release input contains duplicate basenames")
    names = set(by_name)
    archive_name = f"firmware-release-v{batch}.zip"
    required = {archive_name, "npm-index.json"}
    if missing := required - names:
        raise ReleaseError(f"final release input is incomplete: {sorted(missing)}")
    validate_archive(by_name[archive_name], batch)
    try:
        npm_index = json.loads(by_name["npm-index.json"].read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ReleaseError(f"invalid npm index: {error}") from error
    if not isinstance(npm_index, dict):
        raise ReleaseError("npm index must be an object")
    packages = npm_index.get("packages")
    if (
        type(npm_index.get("format")) is not int
        or npm_index["format"] != 1
        or npm_index.get("version") != batch
        or not isinstance(packages, list)
        or type(npm_index.get("package_count")) is not int
        or npm_index["package_count"] != len(packages)
        or not packages
    ):
        raise ReleaseError("npm index identity is invalid")
    npm_assets: set[str] = set()
    package_names: list[str] = []
    for item in packages:
        if (
            not isinstance(item, dict)
            or any(
                not isinstance(item.get(key), str) or not item[key].strip()
                for key in ("name", "version")
            )
            or not isinstance(item.get("sha256"), str)
            or re.fullmatch(r"[0-9a-f]{64}", item["sha256"]) is None
            or type(item.get("size")) is not int
            or item["size"] <= 0
        ):
            raise ReleaseError("npm index contains an invalid entry")
        name = item.get("tarball")
        if (
            not isinstance(name, str)
            or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*\.tgz", name) is None
            or name in npm_assets | required
        ):
            raise ReleaseError(f"invalid or duplicate npm release tarball: {name}")
        npm_assets.add(name)
        package_names.append(item["name"])
    if package_names != sorted(set(package_names)):
        raise ReleaseError("npm index packages must have unique names sorted by name")
    expected = required | npm_assets
    if names != expected:
        raise ReleaseError(
            f"final release inputs differ: missing={sorted(expected - names)}, "
            f"unexpected={sorted(names - expected)}"
        )
    for item in packages:
        source = by_name[item["tarball"]]
        if source.stat().st_size != item["size"] or sha256(source) != item["sha256"]:
            raise ReleaseError(f"npm release tarball integrity mismatch: {source.name}")
    copy_unique(files, output)
    assets = sorted(path for path in output.iterdir() if path.is_file())
    (output / "SHA256SUMS").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in assets),
        encoding="ascii",
    )


def run_slice(
    root: Path,
    bazel: str,
    slice_name: str,
    batch: str,
    input_dir: Path | None,
    output: Path,
) -> None:
    validate_batch(batch)
    if slice_name not in SLICES:
        raise ReleaseError(f"unknown RELEASE_SLICE: {slice_name}")
    if slice_name in PRODUCERS and input_dir is not None:
        raise ReleaseError(f"{slice_name} does not accept RELEASE_INPUT_DIR")
    files = [] if slice_name in PRODUCERS else input_files(input_dir)
    prepare_output(output)
    if slice_name == "catalog":
        build_catalog(root, bazel, output)
    elif slice_name == "npm-packages":
        build_npm_packages(root, bazel, batch, output)
    elif slice_name in FIRMWARE_SLICES:
        build_firmware(root, bazel, slice_name, files, output)
    elif slice_name == "firmware-bundle":
        assert input_dir is not None
        build_firmware_bundle(
            root, bazel, batch, input_dir, files, output
        )
    elif slice_name == "package":
        package_bundle(files, output, batch)
    else:
        assemble_final(files, output, batch)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--slice", default=os.environ.get("RELEASE_SLICE"))
    parser.add_argument("--batch", default=os.environ.get("RELEASE_BATCH"))
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
        if not args.batch:
            raise ReleaseError("RELEASE_BATCH is required")
        input_dir = Path(args.input) if args.input else None
        output_value = Path(args.output) if args.output else None
        output = resolve_output(root, args.slice, output_value)
        run_slice(
            root,
            args.bazel,
            args.slice,
            args.batch,
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
