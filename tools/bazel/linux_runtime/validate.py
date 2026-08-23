#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
from collections.abc import Sequence


class ValidationError(RuntimeError):
    pass


_MAPPED_LIBRARY = re.compile(r"^\s*(\S+)\s+=>\s+(\S+)")
_DIRECT_LIBRARY = re.compile(r"^\s*(/\S+|linux-vdso\.so\.1)\s+")


def parse_ldd(output: str) -> set[str]:
    sonames: set[str] = set()
    for line in output.splitlines():
        if "statically linked" in line or "not a dynamic executable" in line:
            continue
        mapped = _MAPPED_LIBRARY.match(line)
        if mapped is not None:
            soname, target = mapped.groups()
            if target == "not":
                raise ValidationError(f"unresolved dynamic library: {soname}")
            sonames.add(soname)
            continue
        direct = _DIRECT_LIBRARY.match(line)
        if direct is not None:
            sonames.add(pathlib.Path(direct.group(1)).name)
            continue
        if line.strip():
            raise ValidationError(f"unrecognized ldd output: {line.strip()}")
    return sonames


def validate_sonames(sonames: set[str], contract: dict[str, object]) -> None:
    allowed = set(contract.get("allowed_sonames", []))
    forbidden = tuple(
        str(value).lower() for value in contract.get("forbidden_substrings", [])
    )
    forbidden_matches = sorted(
        soname for soname in sonames
        if any(value in soname.lower() for value in forbidden)
    )
    if forbidden_matches:
        raise ValidationError(
            "forbidden dynamic libraries: " + ", ".join(forbidden_matches)
        )
    unknown = sorted(sonames - allowed)
    if unknown:
        raise ValidationError(
            "dynamic libraries are absent from the allowlist: "
            + ", ".join(unknown)
        )


def validate(binary: pathlib.Path, allowlist: pathlib.Path) -> set[str]:
    if not binary.is_file():
        raise ValidationError(f"binary does not exist: {binary}")
    contract = json.loads(allowlist.read_text(encoding="utf-8"))
    completed = subprocess.run(
        ["ldd", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise ValidationError(f"ldd failed with {completed.returncode}: {detail}")
    sonames = parse_ldd(completed.stdout)
    validate_sonames(sonames, contract)
    return sonames


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--allowlist", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        sonames = validate(args.binary, args.allowlist)
    except (OSError, ValueError, ValidationError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for soname in sorted(sonames):
        print(soname)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
