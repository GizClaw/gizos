"""Publication helpers for H2Loader Bazel artifact rules."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
import stat
import sys
import tempfile
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
    recovery_config: Path | dict,
    output: Path,
    board: str,
    target: str,
) -> None:
    config = recovery_config if isinstance(recovery_config, dict) else json.loads(recovery_config.read_text(encoding="utf-8"))
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
        files=[(int(offset, 0), "all-app.bin", recovery_image)],
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


def _protect_secret_process() -> None:
    """Establish crash-dump protection before reading any secret bytes."""
    try:
        import resource
    except ImportError:
        raise ValueError("DUMP_PROTECTION_UNAVAILABLE") from None

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if sys.platform == "linux":
        import ctypes

        # Linux pipe-based core collectors ignore RLIMIT_CORE. Disable and
        # verify dumpability as well; retain it until process exit.
        try:
            prctl = ctypes.CDLL(None, use_errno=True).prctl
        except AttributeError:
            raise ValueError("DUMP_PROTECTION_UNAVAILABLE") from None
        prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
        prctl.restype = ctypes.c_int
        if prctl(4, 0, 0, 0, 0) != 0 or prctl(3, 0, 0, 0, 0) != 0:
            raise ValueError("DUMP_PROTECTION_FAILED")


def read_flash_aes_key(path: Path) -> bytearray:
    """Read exactly 32 raw bytes; never block opening a non-regular input."""
    key = bytearray()
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        with os.fdopen(descriptor, "rb", buffering=0) as secret:
            if not stat.S_ISREG(os.fstat(secret.fileno()).st_mode):
                raise ValueError("KEY_REQUIRES_REGULAR_FILE")
            key.extend(secret.read(33))
        if len(key) != 32:
            raise ValueError("KEY_LENGTH")
        return key
    except BaseException:
        key[:] = b"\0" * len(key)
        raise


def _read_bounded(path: Path, limit: int) -> bytes:
    with path.open("rb") as source:
        data = source.read(limit + 1)
    if not data or len(data) > limit:
        raise ValueError("ENCRYPTION_INPUT_SIZE")
    return data


@dataclass(frozen=True)
class BkEncryptionResult:
    logical_base: int
    source_image_sha256: str
    partition_metadata_sha256: str
    recovery_config: dict | None
    bootloader_contract: str | None = None
    bootloader_receipt_sha256: str | None = None


def _bootloader_identity(partition_metadata: Path, recovery: bytes | None) -> tuple[str | None, str | None]:
    """Return the optional mapped-boot receipt identity; a bad receipt fails closed."""
    path = partition_metadata / "bootloader.json"
    if not os.path.lexists(path):
        return None, None
    data = _read_bounded(path, 1024 * 1024)
    receipt = json.loads(data)
    if (
        not isinstance(receipt, dict)
        or type(receipt.get("schema")) is not int
        or receipt["schema"] != 1
        or receipt.get("contract") != "mapped_ab_v1"
    ):
        raise ValueError("BOOTLOADER_RECEIPT")
    if recovery is not None and receipt.get("native_image_sha256") != hashlib.sha256(recovery).hexdigest():
        raise ValueError("BOOTLOADER_RECEIPT_IMAGE")
    return receipt["contract"], hashlib.sha256(data).hexdigest()


def encrypt_bk_images(
    *,
    key_file: Path,
    partition_metadata: Path,
    app_image: Path,
    app_output: Path,
    recovery_image: Path | None,
    recovery_config: Path | None,
    recovery_output: Path | None,
) -> BkEncryptionResult:
    """Snapshot bounded native inputs before reading the key, then encrypt them.

    An App or Loader encrypts for its own window from its partition metadata.
    Recovery's path-based cipher API receives a frozen copy of the same table;
    publication and receipts never reopen the original unkeyed inputs.
    """
    from tools.bazel.bk7258_flash_aes import encrypt_app, encrypt_recovery, validate_app, validate_layout

    if (recovery_image is None) != (recovery_output is None):
        raise ValueError("encrypted recovery image and output must be declared together")
    if (recovery_image is None) != (recovery_config is None):
        raise ValueError("encrypted recovery requires its layout config")
    settings = None
    recovery = None
    if recovery_image is not None:
        settings = json.loads(_read_bounded(recovery_config, 1024 * 1024))
        if not isinstance(settings, dict) or not isinstance(settings.get("offset"), str) or int(settings["offset"], 0) != 0:
            raise ValueError("RECOVERY_REQUIRES_ZERO_OFFSET")
        if type(settings.get("baud")) is not int or settings["baud"] <= 0:
            raise ValueError("RECOVERY_BAUD")
        recovery = _read_bounded(recovery_image, 16 * 1024 * 1024)
    metadata_bytes = _read_bounded(partition_metadata / "partitions.json", 1024 * 1024)
    metadata = json.loads(metadata_bytes)
    app = _read_bounded(app_image, 16 * 1024 * 1024)
    _, base, _ = validate_app(app, metadata)
    bootloader_contract, bootloader_receipt_sha256 = _bootloader_identity(partition_metadata, recovery)
    with tempfile.TemporaryDirectory(prefix="h2loader-layout-") as temporary:
        frozen_metadata = Path(temporary)
        (frozen_metadata / "partitions.json").write_bytes(metadata_bytes)
        if recovery is not None:
            validate_layout(frozen_metadata, len(recovery))
        _protect_secret_process()
        key = read_flash_aes_key(key_file)
        try:
            encrypted_app = encrypt_app(app, metadata, bytes(key))
            encrypted_recovery = (
                encrypt_recovery(recovery, frozen_metadata, bytes(key))
                if recovery is not None
                else None
            )
        finally:
            key[:] = b"\0" * len(key)
    app_output.parent.mkdir(parents=True, exist_ok=True)
    app_output.write_bytes(encrypted_app)
    if recovery_output is not None:
        recovery_output.parent.mkdir(parents=True, exist_ok=True)
        recovery_output.write_bytes(encrypted_recovery)
    return BkEncryptionResult(
        logical_base=base,
        source_image_sha256=hashlib.sha256(app).hexdigest(),
        partition_metadata_sha256=hashlib.sha256(metadata_bytes).hexdigest(),
        recovery_config=settings,
        bootloader_contract=bootloader_contract,
        bootloader_receipt_sha256=bootloader_receipt_sha256,
    )


def publish_encryption_receipt(
    *,
    output: Path,
    board: str,
    role: str,
    target: str,
    version: str,
    encryption: BkEncryptionResult,
    encrypted_image: Path,
    package: Path,
    recovery: Path | None,
) -> None:
    """Record key-free evidence; a device key match still needs native proof."""
    receipt = {
        "format": "bk7258-flash-aes-package-v1",
        "board": board,
        "role": role,
        "target": target,
        "version": version,
        "logical_base": encryption.logical_base,
        "physical_base": encryption.logical_base // 32 * 34,
        "source_image_sha256": encryption.source_image_sha256,
        "image_sha256": sha256(encrypted_image),
        "package_sha256": sha256(package),
        "partition_metadata_sha256": encryption.partition_metadata_sha256,
    }
    if recovery is not None:
        receipt["recovery_sha256"] = sha256(recovery)
    if encryption.bootloader_contract is not None:
        receipt["bootloader_contract"] = encryption.bootloader_contract
        receipt["bootloader_receipt_sha256"] = encryption.bootloader_receipt_sha256
    output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
