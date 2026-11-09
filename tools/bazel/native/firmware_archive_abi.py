#!/usr/bin/env python3
"""Reject firmware archives that depend on newlib standard-stream state."""

import argparse
import pathlib
import shutil
import subprocess
import sys


FORBIDDEN_SYMBOLS = frozenset({"_impure_ptr", "__getreent"})


def undefined_symbols(nm: str, archive: pathlib.Path) -> set[str]:
    result = subprocess.run(
        [nm, "-u", str(archive)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"nm failed for {archive} ({result.returncode}): {result.stderr.strip()}"
        )
    return {
        line.split()[-1]
        for line in result.stdout.splitlines()
        if line.split()
    }


def validate_archive(nm: str, owner: str, archive: pathlib.Path) -> None:
    forbidden = sorted(undefined_symbols(nm, archive) & FORBIDDEN_SYMBOLS)
    if forbidden:
        symbols = ", ".join(forbidden)
        raise RuntimeError(
            f"{owner}: {archive} has forbidden newlib standard-stream ABI "
            f"dependencies: {symbols}; route diagnostics through h2_pal_log_api_t"
        )


def validate_and_copy(
    nm: str, owner: str, archive: pathlib.Path, output: pathlib.Path
) -> None:
    validate_archive(nm, owner, archive)
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, output)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--owner", required=True)
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        validate_and_copy(args.nm, args.owner, args.input, args.output)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
