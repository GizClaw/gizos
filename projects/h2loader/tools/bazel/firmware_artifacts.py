"""Deterministic H2Loader package and factory-recovery artifact writers."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import io
from pathlib import Path
import posixpath
import re
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


@dataclass(frozen=True)
class ManagedPackage:
    """Validated standard producer output, retained in memory for conversion."""

    manifest: dict[str, str]
    app: bytes
    data: list[BundleEntry]


def _inflate_bounded(data: bytes, limit: int) -> bytes:
    stream = zlib.decompressobj()
    try:
        decoded = stream.decompress(data, limit + 1)
    except zlib.error as error:
        raise ValueError("PACKAGE_COMPRESSION") from error
    if len(decoded) > limit or not stream.eof or stream.unused_data or stream.unconsumed_tail:
        raise ValueError("PACKAGE_COMPRESSION")
    return decoded


def _package_members_unchecked(data: bytes, limit: int) -> dict[str, bytes]:
    """Read regular USTAR members only; never extract to the filesystem."""
    members: dict[str, bytes] = {}
    end = total = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
        for member in archive:
            name = member.name
            if (
                len(members) >= 1027 or not member.isfile()
                or member.type not in (tarfile.REGTYPE, tarfile.AREGTYPE)
                or member.pax_headers or member.offset != end
                or member.offset_data != end + 512
                or not name or name.startswith("/") or "\\" in name
                or posixpath.normpath(name) != name or name == ".."
                or name.startswith("../") or name in members
                or member.size < 0 or member.size > limit - total
            ):
                raise ValueError("PACKAGE_MEMBER")
            validate_tar_header_path(member)
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError("PACKAGE_MEMBER")
            value = stream.read(member.size + 1)
            if len(value) != member.size:
                raise ValueError("PACKAGE_TRUNCATED")
            members[name] = value
            total += member.size
            end = member.offset_data + (member.size + 511) // 512 * 512
    if len(data) - end < 1024 or any(data[end:]):
        raise ValueError("PACKAGE_TRAILING_DATA")
    return members


def _package_members(data: bytes, limit: int) -> dict[str, bytes]:
    try:
        return _package_members_unchecked(data, limit)
    except tarfile.TarError as error:
        raise ValueError("PACKAGE_TAR") from error


def read_managed_package(data: bytes) -> ManagedPackage:
    """Validate bounded format-1/2 producer output for offline conversion.

    This intentionally accepts regular-file USTAR packages, not arbitrary tar
    extensions or filesystem archives. Keys are never needed to inspect it.
    """
    if not data or len(data) > 128 * 1024 * 1024:
        raise ValueError("PACKAGE_SIZE")
    # A standard format-2 package is USTAR; format 1 is one complete zlib stream.
    outer = data if len(data) > 262 and data[257:262] == b"ustar" else _inflate_bounded(data, 96 * 1024 * 1024)
    members = _package_members(outer, 96 * 1024 * 1024)
    text = members.get("manifest", b"")
    if not text or len(text) > 4096 or not text.endswith(b"\n"):
        raise ValueError("PACKAGE_MANIFEST")
    manifest: dict[str, str] = {}
    for line in text.decode("ascii").splitlines():
        key, separator, value = line.partition("=")
        if not separator or not key or key in manifest or not value:
            raise ValueError("PACKAGE_MANIFEST")
        manifest[key] = value
    common = {"format", "role", "board", "target", "version", "image_size", "image_sha256"}
    extra = {"data_sha256", "data_tar_size", "data_bytes", "pixa_bytes", "app_zlib_size", "app_zlib_sha256", "data_zlib_size", "data_zlib_sha256"}
    fmt = manifest.get("format")
    if fmt not in ("1", "2") or set(manifest) != common | (extra if fmt == "2" else set()):
        raise ValueError("PACKAGE_MANIFEST_FIELDS")
    validate_package_identity(manifest["role"], manifest["board"], manifest["target"], manifest["version"])
    for key, value in manifest.items():
        if key.endswith("sha256") and not re.fullmatch("[0-9a-f]{64}", value):
            raise ValueError("PACKAGE_DIGEST")
        if key.endswith(("size", "bytes")) and not re.fullmatch("0|[1-9][0-9]{0,8}", value):
            raise ValueError("PACKAGE_LENGTH")
    if fmt == "2":
        if set(members) != {"manifest", "app.bin.zlib", "data.tar.zlib"}:
            raise ValueError("PACKAGE_MEMBERS")
        for field, name in (("app", "app.bin.zlib"), ("data", "data.tar.zlib")):
            compressed = members[name]
            if len(compressed) != int(manifest[field + "_zlib_size"]) or hashlib.sha256(compressed).hexdigest() != manifest[field + "_zlib_sha256"]:
                raise ValueError("PACKAGE_COMPRESSED_DIGEST")
        app = _inflate_bounded(members["app.bin.zlib"], 16 * 1024 * 1024)
        data_tar = _inflate_bounded(members["data.tar.zlib"], 65 * 1024 * 1024)
        if len(data_tar) != int(manifest["data_tar_size"]):
            raise ValueError("PACKAGE_DATA_LENGTH")
        data_members = _package_members(data_tar, 64 * 1024 * 1024)
        checksum = manifest["data_sha256"]
    else:
        if "app/bk/app_ab_crc.rbl" not in members or "checksum" not in members:
            raise ValueError("PACKAGE_MEMBERS")
        app = members.pop("app/bk/app_ab_crc.rbl")
        checksum_bytes = members.pop("checksum")
        if not re.fullmatch(b"[0-9a-f]{64}\n", checksum_bytes):
            raise ValueError("PACKAGE_DATA_DIGEST")
        checksum = checksum_bytes.decode("ascii").strip()
        members.pop("manifest")
        data_members = members
    if not app or len(app) > 16 * 1024 * 1024 or len(app) != int(manifest["image_size"]) or hashlib.sha256(app).hexdigest() != manifest["image_sha256"]:
        raise ValueError("PACKAGE_IMAGE_DIGEST")
    if len(data_members) > 1024 or sum(map(len, data_members.values())) > 64 * 1024 * 1024:
        raise ValueError("PACKAGE_DATA_LIMIT")
    entries = []
    for name, value in data_members.items():
        if not name.startswith("data/") or data_entry_name(name[5:]) != name:
            raise ValueError("PACKAGE_DATA_PATH")
        entries.append(BundleEntry(name, value))
    if [entry.name for entry in entries] != sorted(data_members) or bundle_checksum(entries) != checksum:
        raise ValueError("PACKAGE_DATA_DIGEST")
    if fmt == "2":
        for field, is_pixa in (("data_bytes", False), ("pixa_bytes", True)):
            if int(manifest[field]) != sum(len(e.data) for e in entries if e.name.endswith(".pixa") == is_pixa):
                raise ValueError("PACKAGE_DATA_LENGTH")
    return ManagedPackage(manifest, app, entries)


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
    package_format: int = 2,
) -> None:
    validate_app_path(app_path)
    if not app_data:
        raise ValueError("missing app payload")
    validate_package_identity(role, board, target, version)
    if type(package_format) is not int or package_format not in (1, 2):
        raise ValueError(f"unsupported package format: {package_format}")
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
        package_format=package_format,
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

    if package_format == 2:
        inner = io.BytesIO()
        with tarfile.open(fileobj=inner, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            for entry in entries:
                add_tar_file(tar, entry.name, entry.data)
        data_tar = inner.getvalue()
        app_zlib = zlib.compress(app_data, level=6)
        data_zlib = zlib.compress(data_tar, level=6)
        manifest += (
            f"data_sha256={checksum}\n"
            f"data_tar_size={len(data_tar)}\n"
            f"data_bytes={sum(len(entry.data) for entry in entries if not entry.name.endswith('.pixa'))}\n"
            f"pixa_bytes={sum(len(entry.data) for entry in entries if entry.name.endswith('.pixa'))}\n"
            f"app_zlib_size={len(app_zlib)}\n"
            f"app_zlib_sha256={hashlib.sha256(app_zlib).hexdigest()}\n"
            f"data_zlib_size={len(data_zlib)}\n"
            f"data_zlib_sha256={hashlib.sha256(data_zlib).hexdigest()}\n"
        ).encode("ascii")
        outer = io.BytesIO()
        with tarfile.open(fileobj=outer, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            add_tar_file(tar, "manifest", manifest)
            add_tar_file(tar, "data.tar.zlib", data_zlib)
            add_tar_file(tar, "app.bin.zlib", app_zlib)
        out_path.write_bytes(outer.getvalue())
        return

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
    package_format: int = 2,
) -> dict[str, str | int]:
    if not app_data:
        raise ValueError("missing app payload")
    validate_package_identity(role, board, target, version)
    if type(package_format) is not int or package_format not in (1, 2):
        raise ValueError(f"unsupported package format: {package_format}")
    return {
        "format": package_format,
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
