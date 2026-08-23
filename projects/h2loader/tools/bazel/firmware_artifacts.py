"""Deterministic H2Loader package and factory-recovery artifact writers."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import io
from pathlib import Path
import posixpath
import struct
import tarfile
import zlib


FACTORY_FILE_CAPACITY = 16
FACTORY_NAME_SIZE = 128
FACTORY_IDENTITY_SIZE = 96
FACTORY_HEADER_SIZE = (
    4 + 2 + 2 + 4 + 4 + 4 + 2 * FACTORY_IDENTITY_SIZE
    + FACTORY_FILE_CAPACITY * (4 + 8 + 8 + 32 + FACTORY_NAME_SIZE)
)


@dataclass(frozen=True)
class BundleEntry:
    name: str
    data: bytes


def write_package(
    out_path: Path,
    app_path: str,
    app_data: bytes,
    data_entries: list[BundleEntry],
    *,
    role: str,
    board: str,
    target: str,
    version: str,
) -> None:
    validate_app_path(app_path)
    if not app_data:
        raise ValueError("missing app payload")
    validate_package_identity(role, board, target, version)
    if role == "h2loader" and data_entries:
        raise ValueError("h2loader packages cannot contain data entries")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    entries = sorted(data_entries, key=lambda entry: entry.name)
    checksum = bundle_checksum(entries)
    metadata = package_manifest(
        app_data,
        role=role,
        board=board,
        target=target,
        version=version,
    )
    manifest = (
        f"format={metadata['format']}\n"
        f"role={metadata['role']}\n"
        f"board={metadata['board']}\n"
        f"target={metadata['target']}\n"
        f"version={metadata['version']}\n"
        f"image_size={metadata['image_size']}\n"
        f"image_sha256={metadata['image_sha256']}\n"
    ).encode("ascii")

    tar_data = io.BytesIO()
    with tarfile.open(fileobj=tar_data, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        add_tar_file(tar, "manifest", manifest)
        add_tar_file(tar, "checksum", checksum.encode() + b"\n")
        for entry in entries:
            add_tar_file(tar, entry.name, entry.data)
        add_tar_file(tar, app_path, app_data)
    out_path.write_bytes(zlib.compress(tar_data.getvalue(), level=6))


def package_manifest(
    app_data: bytes,
    *,
    role: str,
    board: str,
    target: str,
    version: str,
) -> dict[str, str | int]:
    if not app_data:
        raise ValueError("missing app payload")
    validate_package_identity(role, board, target, version)
    return {
        "format": 1,
        "role": role,
        "board": board,
        "target": target,
        "version": version,
        "image_size": len(app_data),
        "image_sha256": hashlib.sha256(app_data).hexdigest(),
    }


def write_factory_bundle(
    out_path: Path,
    *,
    driver: int,
    board: str,
    target: str,
    baud: int,
    files: list[tuple[int, str, Path]],
) -> None:
    if driver not in (1, 2) or not isinstance(baud, int) or baud <= 0:
        raise ValueError(f"invalid factory driver or baud for {out_path}")
    if not 0 < len(files) <= FACTORY_FILE_CAPACITY:
        raise ValueError(f"invalid factory file count for {out_path}")
    board_bytes = board.encode("ascii")
    target_bytes = target.encode("ascii")
    if len(board_bytes) >= FACTORY_IDENTITY_SIZE or len(target_bytes) >= FACTORY_IDENTITY_SIZE:
        raise ValueError(f"factory identity is too long for {out_path}")

    names: set[str] = set()
    flash_ranges: list[tuple[int, int]] = []
    data_offset = FACTORY_HEADER_SIZE
    records = bytearray()
    payloads: list[bytes] = []
    for flash_offset, name, source in files:
        if (
            not isinstance(flash_offset, int)
            or flash_offset < 0
            or flash_offset > 0xFFFFFFFF
            or not name
            or Path(name).name != name
            or name in names
        ):
            raise ValueError(f"invalid factory member for {out_path}: {name}")
        names.add(name)
        name_bytes = name.encode("ascii")
        if len(name_bytes) >= FACTORY_NAME_SIZE:
            raise ValueError(f"factory member name is too long: {name}")
        payload = source.read_bytes()
        if not payload:
            raise ValueError(f"factory member is empty: {source}")
        if driver == 1 and (flash_offset % 4 != 0 or len(payload) % 4 != 0):
            raise ValueError(f"ESP factory member offset and size must be 4-byte aligned: {name}")
        flash_end = flash_offset + len(payload)
        if flash_end > 0x100000000 or any(
            flash_offset < previous_end and previous_start < flash_end
            for previous_start, previous_end in flash_ranges
        ):
            raise ValueError(f"overlapping or overflowing factory member: {name}")
        flash_ranges.append((flash_offset, flash_end))
        records.extend(struct.pack(
            "<IQQ32s128s",
            flash_offset,
            data_offset,
            len(payload),
            hashlib.sha256(payload).digest(),
            name_bytes.ljust(FACTORY_NAME_SIZE, b"\0"),
        ))
        payloads.append(payload)
        data_offset += len(payload)

    record_size = 4 + 8 + 8 + 32 + FACTORY_NAME_SIZE
    records.extend(b"\0" * ((FACTORY_FILE_CAPACITY - len(files)) * record_size))
    header = (
        b"H2FB"
        + struct.pack("<HHIII", 1, driver, 1, baud, len(files))
        + board_bytes.ljust(FACTORY_IDENTITY_SIZE, b"\0")
        + target_bytes.ljust(FACTORY_IDENTITY_SIZE, b"\0")
        + records
    )
    if len(header) != FACTORY_HEADER_SIZE:
        raise ValueError(f"factory header size mismatch for {out_path}")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(header + b"".join(payloads))


def validate_package_identity(role: str, board: str, target: str, version: str) -> None:
    if role not in ("app", "h2loader"):
        raise ValueError(f"unsupported image role: {role}")
    for name, value in (("board", board), ("target", target)):
        if not value or any(character not in "abcdefghijklmnopqrstuvwxyz0123456789._-" for character in value):
            raise ValueError(f"invalid {name}: {value}")
    if not version or len(version.encode("ascii", errors="strict")) > 95:
        raise ValueError("version must contain 1..95 ASCII bytes")
    if any(ord(character) < 0x20 or ord(character) > 0x7E for character in version):
        raise ValueError("version must be printable ASCII")
    if any(character.isspace() for character in version):
        raise ValueError("version must not contain whitespace")


def read_data_dir(data_dir: str | None) -> list[BundleEntry]:
    if not data_dir:
        return []
    root = Path(data_dir)
    if not root.is_dir():
        raise ValueError(f"data directory does not exist: {root}")
    return [
        BundleEntry(name=data_entry_name(path.relative_to(root).as_posix()), data=path.read_bytes())
        for path in sorted(path for path in root.rglob("*") if path.is_file())
    ]


def data_entry_name(relative: str) -> str:
    if (
        not relative
        or relative.startswith("/")
        or "\\" in relative
        or posixpath.normpath(relative) != relative
        or relative == ".."
        or relative.startswith("../")
    ):
        raise ValueError(f"invalid data path {relative!r}")
    return posixpath.join("data", relative)


def bundle_checksum(entries: list[BundleEntry]) -> str:
    digest = hashlib.sha256()
    for entry in entries:
        digest.update(entry.name.encode())
        digest.update(b"\0")
        digest.update(entry.data)
        digest.update(b"\0")
    return digest.hexdigest()


def add_tar_file(tar: tarfile.TarFile, name: str, data: bytes) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    info.mtime = 0
    validate_tar_header_path(info)
    tar.addfile(info, fileobj=io.BytesIO(data))


def validate_tar_header_path(info: tarfile.TarInfo) -> None:
    try:
        info.tobuf(format=tarfile.USTAR_FORMAT)
    except ValueError as error:
        raise ValueError(f"unsupported tar path {info.name!r}: {error}") from error


def validate_app_path(app_path: str) -> None:
    if (
        not app_path
        or app_path.startswith("/")
        or "\\" in app_path
        or posixpath.normpath(app_path) != app_path
        or not app_path.startswith("app/")
    ):
        raise ValueError(f"invalid app path {app_path!r}")
