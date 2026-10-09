"""Build a deterministic Lua app archive and its release metadata."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
import zlib

from embed_resource import compact_source

APP_ID = re.compile(r"^[a-z0-9_-][a-z0-9_.-]{0,31}$")
SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


def json_bytes(value: object) -> bytes:
    return (
        json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_package(
    specification: dict, archive: Path, manifest: Path, metadata: Path
) -> None:
    app_id, version, entry = (
        specification.get(key) for key in ("app_id", "version", "entry")
    )
    if not isinstance(app_id, str) or not APP_ID.fullmatch(app_id):
        raise ValueError("Invalid Lua app id")
    if (
        not isinstance(version, str)
        or len(version) > 31
        or not SEMVER.fullmatch(version)
    ):
        raise ValueError("Invalid declared Lua app SemVer")
    if entry != app_id + ".lua":
        raise ValueError("Lua app entry must match its app id")
    files = specification.get("files")
    if not isinstance(files, dict) or entry not in files:
        raise ValueError("Lua app package must contain its entry")
    contents: dict[str, bytes] = {}
    for name, source in sorted(files.items()):
        if not isinstance(name, str) or not isinstance(source, str):
            raise ValueError("Lua app files must map package paths to source paths")
        path = PurePosixPath(name)
        if (
            path.is_absolute()
            or str(path) != name
            or "\\" in name
            or any(part in {".", ".."} or part.startswith(".") for part in path.parts)
            or any(ord(character) < 32 or ord(character) == 127 for character in name)
            or name != entry
            and not name.startswith(app_id + "/")
        ):
            raise ValueError("Invalid Lua app package path: " + name)
        contents[name] = Path(source).read_bytes()
    portable_names = {name.casefold() for name in contents}
    if len(portable_names) != len(contents):
        raise ValueError("Lua app package has case-insensitive path collisions")
    for name in contents:
        if any(
            str(parent).casefold() in portable_names
            for parent in PurePosixPath(name).parents
        ):
            raise ValueError("Lua app package has a file/directory path collision")
    source_text = contents[entry]
    if b"\0" in source_text or source_text.startswith(b"\x1bLua"):
        raise ValueError(
            "Lua app entry must be text, not bytecode or NUL-containing data"
        )
    compact = specification.get("compact", False)
    if type(compact) is not bool:
        raise ValueError("Lua app compact flag must be boolean")
    if compact:
        contents[entry] = compact_source(source_text)
    value = {
        "format": 1,
        "type": "lua-app",
        "app_id": app_id,
        "version": version,
        "entry": entry,
        "data_dir": app_id if len(contents) > 1 else None,
        "compact": compact,
        "files": [
            {"path": name, "size": len(data), "sha256": digest(data)}
            for name, data in contents.items()
        ],
    }
    manifest_bytes = json_bytes(value)
    contents["manifest.json"] = manifest_bytes
    # USTAR, zero timestamps and fixed ownership avoid machine/run provenance.
    with io.BytesIO() as tar_bytes:
        with tarfile.open(
            fileobj=tar_bytes, mode="w", format=tarfile.USTAR_FORMAT
        ) as package:
            for name, data in sorted(contents.items()):
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o644
                package.addfile(info, io.BytesIO(data))
        archive.write_bytes(zlib.compress(tar_bytes.getvalue(), level=9))
    manifest.write_bytes(manifest_bytes)
    metadata.write_bytes(
        json_bytes(
            {
                "format": 1,
                "type": "lua-app",
                "app_id": app_id,
                "version": version,
                "entry": entry,
                "data_dir": value["data_dir"],
                "compact": compact,
                "manifest": value,
                "package": {
                    "name": archive.name,
                    "size": archive.stat().st_size,
                    "sha256": digest(archive.read_bytes()),
                },
            }
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("specification", "archive", "manifest", "metadata"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    try:
        specification = json.loads(args.specification.read_text(encoding="utf-8"))
        if not isinstance(specification, dict):
            raise ValueError("Lua app specification must be an object")
        build_package(specification, args.archive, args.manifest, args.metadata)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Lua app package: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
