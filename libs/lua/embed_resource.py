#!/usr/bin/env python3
"""Convert one text Lua source into deterministic C source and header files."""

import argparse
import pathlib
import re


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--header", type=pathlib.Path, required=True)
    parser.add_argument("--implementation", type=pathlib.Path, required=True)
    parser.add_argument("--symbol", required=True)
    args = parser.parse_args()
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.symbol) is None:
        parser.error("symbol must be a C identifier")
    data = args.source.read_bytes()
    if data.startswith(b"\x1b"):
        parser.error("Lua bytecode cannot be embedded")
    header_guard = f"{args.symbol.upper()}_H"
    args.header.write_text(
        "\n".join(
            [
                f"#ifndef {header_guard}",
                f"#define {header_guard}",
                "",
                "#include <stddef.h>",
                "#include <stdint.h>",
                "",
                f"extern const uint8_t {args.symbol}[];",
                f"extern const size_t {args.symbol}_size;",
                "",
                f"#endif  /* {header_guard} */",
                "",
            ]
        ),
        encoding="utf-8",
    )
    rows = []
    for start in range(0, len(data), 16):
        rows.append("  " + ", ".join(str(byte) for byte in data[start : start + 16]) + ",")
    args.implementation.write_text(
        "\n".join(
            [
                f'#include "{args.header.name}"',
                "",
                f"const uint8_t {args.symbol}[] = {{",
                *rows,
                "};",
                f"const size_t {args.symbol}_size = sizeof({args.symbol});",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
