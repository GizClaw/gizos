"""Publication helpers for H2Loader Bazel artifact rules."""

from __future__ import annotations

import hashlib
import json
import struct
import sys
from pathlib import Path

from projects.h2loader.tools.bazel.firmware_artifacts import (
    BundleEntry,
    data_entry_name,
    package_manifest,
    write_factory_bundle,
    write_package,
)


GIT_LFS_POINTER_HEADER = b"version https://git-lfs.github.com/spec/v1"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def package_entries(
    source_root: Path,
    data_root: str,
    files: list[str],
    sources: list[str] | None = None,
) -> list[BundleEntry]:
    """Bundles package data under `data_root`.

    `files` holds each entry's repository-relative name. `sources` holds where
    its bytes are, one per file and in the same order; a generated file is read
    from its output location while it keeps its repository-relative name, so a
    rule may produce package data instead of requiring a checked-in copy under
    the declared root. Omitting `sources` reads each file from its own name.
    """
    if sources is None:
        sources = list(files)
    # Check the pairing before the empty shortcut: sources without files means
    # the caller lost the correspondence, and silently packaging nothing would
    # hide it.
    if len(sources) != len(files):
        raise ValueError(
            f"package data needs one source per file: {len(files)} files, {len(sources)} sources"
        )
    if not files:
        return []
    root = Path(data_root)
    if root.is_absolute() or ".." in root.parts:
        raise ValueError(f"package data root must be repository-relative: {data_root}")
    entries: list[BundleEntry] = []
    names: set[str] = set()
    for value, read_value in zip(files, sources):
        logical_source = Path(value)
        if logical_source.is_absolute() or not read_value:
            raise ValueError(f"package data escapes declared root {data_root}: {value}")
        try:
            relative_path = logical_source.relative_to(root)
        except ValueError as error:
            raise ValueError(f"package data escapes declared root {data_root}: {value}") from error
        if not relative_path.parts or ".." in relative_path.parts:
            raise ValueError(f"package data escapes declared root {data_root}: {value}")
        source = source_root / Path(read_value)
        if not source.is_file():
            raise ValueError(f"package data file is missing: {value}")
        data = source.read_bytes()
        if data.splitlines()[:1] == [GIT_LFS_POINTER_HEADER]:
            raise ValueError(
                f"package data is an unresolved Git LFS pointer: {value}; "
                "materialize Git LFS objects before packaging"
            )
        name = data_entry_name(relative_path.as_posix())
        if name in names:
            raise ValueError(f"duplicate package data path: {name}")
        names.add(name)
        entries.append(BundleEntry(name=name, data=data))
    return entries


def publish_managed_package(
    *,
    source_root: Path,
    app_image: Path,
    app_path: str,
    data_root: str,
    data_files: list[str],
    data_sources: list[str],
    output: Path,
    board: str,
    role: str,
    target: str,
    version: str,
    package_format: int = 2,
) -> None:
    entries = package_entries(source_root, data_root, data_files, data_sources)
    write_package(
        output,
        app_path,
        app_image.read_bytes(),
        entries,
        role=role,
        board=board,
        target=target,
        version=version,
        package_format=package_format,
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
    platform: str,
    board: str,
    image: str,
    role: str,
    target: str,
    version: str,
    app_image: Path,
    package: Path,
    factory: Path | None,
    recovery: Path | None,
    native: list[tuple[str, Path]],
    package_format: int = 2,
) -> None:
    def asset(
        path: Path,
        operation: str,
        release_suffix: str | None,
        name: str | None = None,
        flash_offset: int | None = None,
    ) -> dict[str, str | int]:
        asset_name = name or path.name
        normalized = Path(asset_name)
        if normalized.is_absolute() or ".." in normalized.parts:
            raise ValueError(f"invalid artifact name: {asset_name}")
        result: dict[str, str | int] = {
            "name": normalized.as_posix(),
            "operation": operation,
            "sha256": sha256(path),
            "size": path.stat().st_size,
        }
        if release_suffix is not None:
            result["release_suffix"] = release_suffix
        if flash_offset is not None:
            result["flash_offset"] = flash_offset
        return result

    if type(package_format) is not int or package_format not in (1, 2):
        raise ValueError(f"unsupported package format: {package_format}")
    suffix = ".update.tar" if package_format == 2 else ".update.tar.zlib"
    release_assets = [asset(package, "managed-install", suffix)]
    if recovery is not None:
        release_assets.append(asset(recovery, "recovery", ".recovery.h2fb"))
    if factory is not None:
        release_assets.append(
            asset(
                factory,
                "factory-flash",
                ".combined_factory.bin",
                flash_offset=0,
            )
        )
    metadata = {
        "entry": entry,
        "platform": platform,
        "board": board,
        "image": image,
        "role": role,
        "target": target,
        "version": version,
        "package_format": package_format,
        "package_manifest": package_manifest(
            app_image.read_bytes(),
            role=role,
            board=board,
            target=target,
            version=version,
            package_format=package_format,
        ),
        "assets": release_assets,
        "native_artifacts": [
            asset(path, "native-debug-or-flash", None, name)
            for name, path in native
        ],
    }
    names = [item["name"] for item in metadata["native_artifacts"]]
    if len(names) != len(set(names)):
        raise ValueError("native artifact metadata contains duplicate names")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _recovery_message(message: str, *, error: bool = False) -> None:
    """Diagnostics cannot change publication state or expose an I/O exception."""
    try:
        print(message, file=sys.stderr if error else sys.stdout)
    except (OSError, ValueError):
        pass


def _read_recovery_key(fd: int, timeout: float = 30.0) -> bytearray:
    """Read one bounded EOF-terminated secret from an owned pipe/socket stream."""
    import os
    import selectors
    import stat
    import time

    mode = os.fstat(fd).st_mode
    if not (stat.S_ISFIFO(mode) or stat.S_ISSOCK(mode)):
        raise ValueError("KEY_REQUIRES_PIPE_OR_SOCKET")
    key = bytearray()
    try:
        deadline = time.monotonic() + timeout
        with os.fdopen(os.dup(fd), "rb", buffering=0) as secret, selectors.DefaultSelector() as readiness:
            readiness.register(secret, selectors.EVENT_READ)
            while len(key) < 33:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not readiness.select(remaining):
                    raise ValueError("KEY_INPUT_TIMEOUT")
                chunk = os.read(secret.fileno(), 33 - len(key))
                if not chunk:
                    break
                key.extend(chunk)
        if len(key) != 32:
            raise ValueError("KEY_LENGTH")
        return key
    except BaseException:
        key[:] = b"\0" * len(key)
        raise


def encrypted_bk_recovery_main(argv: list[str] | None = None) -> int:
    """Explicit local operation; stdin/descriptor carries exactly 32 raw key bytes."""
    import argparse
    import os
    import resource
    import shutil
    import tempfile

    from tools.bazel.bk7258_flash_aes import encrypt_recovery

    parser = argparse.ArgumentParser(description="Create offline encrypted BK7258 AB recovery; no device access", allow_abbrev=False)
    parser.add_argument("--source", required=True)
    parser.add_argument("--metadata", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--target", choices=["bk7258"], required=True)
    parser.add_argument("--key-fd", type=int, required=True, help="Runtime descriptor containing exactly 32 raw bytes, then EOF; 0 uses stdin")
    parser.add_argument("--output-dir", required=True, help="New absolute local output directory, outside Bazel outputs")
    tokens = sys.argv[1:] if argv is None else argv
    for flag in ("--source", "--metadata", "--config", "--board", "--target", "--key-fd", "--output-dir"):
        if sum(token == flag or token.startswith(flag + "=") for token in tokens) > 1:
            parser.error("duplicate option")
    args = parser.parse_args(tokens)
    temporary = None
    key = bytearray()
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        output = Path(args.output_dir)
        if not output.is_absolute() or output.exists() or output.is_symlink():
            raise ValueError("OUTPUT_DIRECTORY")
        parent = output.parent.resolve(strict=True)
        # Refuse declared Bazel output trees; keyed results are runtime-only.
        if any(part in ("bazel-out", "execroot") or part.endswith(".runfiles") for part in parent.parts):
            raise ValueError("BAZEL_OUTPUT_DIRECTORY")
        config = Path(args.config)
        settings = json.loads(config.read_text(encoding="utf-8"))
        if not isinstance(settings, dict) or not isinstance(settings.get("offset"), str) or int(settings["offset"], 0) != 0:
            raise ValueError("RECOVERY_REQUIRES_ZERO_OFFSET")
        if type(settings.get("baud")) is not int or settings["baud"] <= 0:
            raise ValueError("RECOVERY_BAUD")
        key = _read_recovery_key(args.key_fd)
        if not 0 < Path(args.source).stat().st_size <= 16 * 1024 * 1024:
            raise ValueError("SOURCE_LENGTH")
        source_bytes = Path(args.source).read_bytes()
        encrypted = encrypt_recovery(source_bytes, Path(args.metadata), bytes(key))
        temporary = Path(tempfile.mkdtemp(prefix=".bk-recovery-", dir=parent))
        image = temporary / "encrypted.bin"
        with image.open("xb") as stream:
            os.chmod(image, 0o600)
            stream.write(encrypted)
        frozen_config = temporary / ".recovery-config.json"
        frozen_config.write_text(json.dumps(settings), encoding="utf-8")
        publish_bk_recovery(
            recovery_image=image, recovery_config=frozen_config,
            output=temporary / "recovery.h2fb", board=args.board, target=args.target,
        )
        frozen_config.unlink()
        os.chmod(temporary / "recovery.h2fb", 0o600)
        receipt = {
            "format": "bk7258-encrypted-recovery-v1",
            "board": args.board,
            "target": args.target,
            "source_sha256": hashlib.sha256(source_bytes).hexdigest(),
            "encrypted_sha256": sha256(image),
            "bundle_sha256": sha256(temporary / "recovery.h2fb"),
        }
        receipt_path = temporary / "validation.json"
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        os.chmod(receipt_path, 0o600)
        if output.exists() or output.is_symlink():
            raise ValueError("OUTPUT_DIRECTORY")
        temporary.rename(output)
        temporary = None
        # Publication already committed. A closed logging consumer must not
        # turn a complete output into a reported validation failure.
        _recovery_message("Encrypted recovery created; hardware/key matching is not verified.")
        return 0
    except (OSError, ValueError, KeyError, TypeError, struct.error):
        # Never expose exception details, input values or a secret-bearing traceback.
        _recovery_message("Encrypted recovery refused: invalid input, secret descriptor or output.", error=True)
        return 1
    finally:
        key[:] = b"\0" * len(key)
        if temporary is not None:
            try:
                shutil.rmtree(temporary)
            except OSError:
                _recovery_message("Private recovery staging cleanup failed; no output was published.", error=True)


if __name__ == "__main__":
    raise SystemExit(encrypted_bk_recovery_main())
