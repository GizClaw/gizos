"""Validate an externally managed CPython before exposing it to Bazel."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import re
import struct
import sys
import sysconfig


class RuntimeValidationError(RuntimeError):
    pass


def runtime_info() -> dict[str, object]:
    interpreter = Path(sys._base_executable).resolve()
    with interpreter.open("rb") as stream:
        header = stream.read(20)
    elf_machine = None
    if header[:6] == b"\x7fELF\x02\x01" and len(header) == 20:
        elf_machine = int.from_bytes(header[18:20], "little")
    return {
        "version": ".".join(str(part) for part in sys.version_info[:3]),
        "implementation": sys.implementation.name,
        "abi_flags": sys.abiflags,
        "cache_tag": sys.implementation.cache_tag,
        "soabi": sysconfig.get_config_var("SOABI"),
        "extension_suffix": sysconfig.get_config_var("EXT_SUFFIX"),
        "pointer_bits": struct.calcsize("P") * 8,
        "byteorder": sys.byteorder,
        "platform": sys.platform,
        "machine": platform.machine().lower(),
        "elf_machine": elf_machine,
        "interpreter": str(interpreter),
        "prefix": str(Path(sys.base_prefix).resolve()),
    }


def validate_runtime(
    info: dict[str, object],
    version: str,
    requested: str,
    package_root: Path,
    host_os: str = "linux",
    host_arch: str = "aarch64",
) -> None:
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) or version.rsplit(".", 1)[0] != requested:
        raise RuntimeValidationError(f"package version {version!r} does not satisfy Python {requested}")
    expected = {
        "version": version,
        "implementation": "cpython",
        "abi_flags": "",
        "cache_tag": "cpython-" + requested.replace(".", ""),
        "pointer_bits": 64,
        "byteorder": "little",
        "platform": host_os,
    }
    for name, value in expected.items():
        if info.get(name) != value:
            raise RuntimeValidationError(f"managed Python {name} mismatch: expected {value!r}, found {info.get(name)!r}")
    normalize_arch = lambda value: "aarch64" if value in ("arm64", "aarch64") else value
    if normalize_arch(info.get("machine")) != normalize_arch(host_arch):
        raise RuntimeValidationError(f"managed Python machine mismatch: expected {host_arch}, found {info.get('machine')}")
    if host_os == "linux" and normalize_arch(host_arch) == "aarch64":
        soabi = f"cpython-{requested.replace('.', '')}-aarch64-linux-gnu"
        if info.get("soabi") != soabi or info.get("extension_suffix") != f".{soabi}.so":
            raise RuntimeValidationError(f"managed Python ABI mismatch: expected {soabi}, found {info.get('soabi')}")
        if info.get("elf_machine") != 183:
            raise RuntimeValidationError("managed Python must be a native ELF64 little-endian AArch64 executable")
    root = package_root.resolve()
    for name in ("interpreter", "prefix"):
        raw = info.get(name)
        if not isinstance(raw, str) or not Path(raw).is_absolute() or not Path(raw).resolve().is_relative_to(root):
            raise RuntimeValidationError(f"managed Python {name} escapes package prefix {root}: {raw!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--requested", required=True)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--host-os", default="linux")
    parser.add_argument("--host-arch", default="aarch64")
    arguments = parser.parse_args()
    try:
        info = runtime_info()
        validate_runtime(info, arguments.version, arguments.requested, arguments.package_root,
                         arguments.host_os, arguments.host_arch)
    except (OSError, RuntimeValidationError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(info, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
