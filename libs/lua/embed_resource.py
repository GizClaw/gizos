#!/usr/bin/env python3
"""Convert one text Lua source into deterministic C source and header files."""

import argparse
import pathlib
import re

_LONG_BRACKET = re.compile(rb"\[(=*)\[")
_LINE_BREAK = re.compile(rb"\r\n|\n\r|\r|\n")


def compact_source(data: bytes) -> bytes:
    """Remove comments/extra horizontal space, retaining literals/line numbers.

    Always keep a separator where a comment or horizontal whitespace separated
    tokens. This deliberately does not rename identifiers or rewrite Lua tokens.
    """
    if b"\0" in data:
        # The text loader rejects NUL even inside comments. Do not erase one
        # and turn a rejected source into an accepted chunk.
        raise ValueError("Lua source contains an embedded NUL")
    output = bytearray()
    pending_space = False
    index = 0
    while index < len(data):
        byte = data[index]
        if byte in b" \t\v\f":
            pending_space = True
            index += 1
            continue
        comment = data.startswith(b"--", index)
        start = index + 2 if comment else index
        bracket = _LONG_BRACKET.match(data, start)
        if comment:
            if bracket:
                end_marker = b"]" + bracket[1] + b"]"
                end = data.find(end_marker, start + len(bracket[0]))
                if end < 0:
                    raise ValueError("unterminated Lua long comment")
                end += len(end_marker)
            else:
                end = index + 2
                while end < len(data) and data[end] not in b"\r\n":
                    end += 1
            # Lua treats CRLF/LFCR as one newline. Normalize outside literals
            # so removing comments/space cannot accidentally join two breaks.
            output.extend(b"\n" * len(_LINE_BREAK.findall(data[index:end])))
            pending_space = True
            index = end
            continue
        if byte in b"\r\n":
            output.append(ord("\n"))
            pending_space = False
            index += 1
            if index < len(data) and data[index] in b"\r\n" and data[index] != byte:
                index += 1
            continue
        if pending_space and output and output[-1] not in b"\r\n":
            output.append(ord(" "))
        pending_space = False
        if bracket:
            end_marker = b"]" + bracket[1] + b"]"
            end = data.find(end_marker, index + len(bracket[0]))
            if end < 0:
                raise ValueError("unterminated Lua long string")
            end += len(end_marker)
        elif byte in b"'\"":
            end = index + 1
            while end < len(data) and data[end] != byte:
                # Copy escaped characters verbatim, including escaped quotes,
                # newlines and all whitespace following Lua's \\z escape.
                end += 2 if data[end] == ord("\\") else 1
            if end >= len(data):
                raise ValueError("unterminated Lua short string")
            end += 1
        else:
            end = index + 1
        output.extend(data[index:end])
        index = end
    return bytes(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--header", type=pathlib.Path, required=True)
    parser.add_argument("--implementation", type=pathlib.Path, required=True)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.symbol) is None:
        parser.error("symbol must be a C identifier")
    data = args.source.read_bytes()
    if data.startswith(b"\x1b"):
        parser.error("Lua bytecode cannot be embedded")
    if args.compact:
        try:
            data = compact_source(data)
        except ValueError as error:
            parser.error(str(error))
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
    if not data:
        rows.append("  0,")  # ISO C has no zero-length array; logical size stays 0.
    source_size = f"sizeof({args.symbol})" if data else "0u"
    args.implementation.write_text(
        "\n".join(
            [
                f'#include "{args.header.name}"',
                "",
                f"const uint8_t {args.symbol}[] = {{",
                *rows,
                "};",
                f"const size_t {args.symbol}_size = {source_size};",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
