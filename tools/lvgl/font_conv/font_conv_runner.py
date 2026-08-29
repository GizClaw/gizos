"""Run the pinned LVGL font converter over declared UTF-8 symbol sources."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unicodedata


def collect_symbols(paths: list[Path]) -> str:
    """Return stable unique non-control characters from UTF-8 text files."""
    symbols: set[str] = set()
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="strict")
        symbols.update(
            character
            for character in text
            if not unicodedata.category(character).startswith("C")
        )
    return "".join(sorted(symbols, key=ord))


def convert(args: argparse.Namespace) -> None:
    """Invoke lv_font_conv and publish only a complete non-empty C source."""
    output = Path(args.output).resolve()
    symbols = collect_symbols([Path(path) for path in args.symbol_source])
    if not symbols and not args.ranges:
        raise ValueError("font conversion requires symbols or ranges")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lvgl-font-", dir=output.parent) as directory:
        temporary_output = Path(directory) / output.name
        if args.converter:
            command = [str(Path(args.converter).resolve())]
        elif args.node and args.converter_entry:
            if not args.converter_package:
                raise ValueError("direct Node conversion requires --converter-package")
            command = [
                str(Path(args.node).resolve()),
                "--preserve-symlinks-main",
                str(Path(args.converter_entry)),
                str(Path(args.converter_package)),
            ]
        else:
            raise ValueError(
                "font conversion requires --converter or both --node and "
                "--converter-entry"
            )
        command.extend(
            [
            "--font",
            str(Path(args.font).resolve()),
            ]
        )
        if symbols:
            command.extend(["--symbols", symbols])
        for value in args.ranges:
            command.extend(["--range", value])
        command.extend(
            [
                "--size",
                str(args.size),
                "--bpp",
                str(args.bpp),
                "--format",
                "lvgl",
                "--lv-font-name",
                args.font_name,
                "--lv-include",
                args.lv_include,
            ]
        )
        if not args.compressed:
            command.extend(["--no-compress", "--no-prefilter"])
        command.extend(["--output", str(temporary_output)])
        subprocess.run(command, check=True)
        if not temporary_output.is_file() or temporary_output.stat().st_size == 0:
            raise RuntimeError("lv_font_conv did not produce a non-empty C source")
        os.replace(temporary_output, output)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--converter")
    parser.add_argument("--node")
    parser.add_argument("--converter-entry")
    parser.add_argument("--converter-package")
    parser.add_argument("--font", required=True)
    parser.add_argument("--symbol-source", action="append", default=[])
    parser.add_argument("--range", dest="ranges", action="append", default=[])
    parser.add_argument("--size", required=True, type=int)
    parser.add_argument("--bpp", required=True, type=int)
    parser.add_argument("--compressed", action="store_true")
    parser.add_argument("--font-name", required=True)
    parser.add_argument("--lv-include", required=True)
    parser.add_argument("--output", required=True)
    return parser.parse_args(argv)


def main() -> None:
    convert(parse_args())


if __name__ == "__main__":
    main()
