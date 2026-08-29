"""Create a deterministic TTF subset and emit it as C data."""

import argparse
import io
import re
import unicodedata
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont


IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def parse_range(value: str) -> range:
    parts = value.split("-", 1)
    start = int(parts[0], 0)
    end = int(parts[1], 0) if len(parts) == 2 else start
    if start < 0 or end < start or end > 0x10FFFF:
        raise ValueError(f"invalid Unicode range: {value}")
    return range(start, end + 1)


def collect_codepoints(paths: list[Path], ranges: list[str]) -> frozenset[int]:
    codepoints: set[int] = set()
    for path in paths:
        for character in path.read_text(encoding="utf-8"):
            if not unicodedata.category(character).startswith("C"):
                codepoints.add(ord(character))
    for value in ranges:
        codepoints.update(parse_range(value))
    if not codepoints:
        raise ValueError("TTF subset requires symbols or ranges")
    return frozenset(codepoints)


def subset_font(source: bytes, codepoints: frozenset[int]) -> bytes:
    options = subset.Options()
    options.hinting = False
    options.name_IDs = []
    options.name_languages = []
    options.recalc_timestamp = False
    font = subset.load_font(io.BytesIO(source), options, dontLoadGlyphNames=True)
    font.recalcTimestamp = False
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=sorted(codepoints))
    subsetter.subset(font)
    output = io.BytesIO()
    subset.save_font(font, output, options)
    return output.getvalue()


def font_codepoints(data: bytes) -> frozenset[int]:
    font = TTFont(io.BytesIO(data), lazy=False)
    try:
        return frozenset(font.getBestCmap())
    finally:
        font.close()


def render_header(symbol: str) -> str:
    guard = f"{symbol.upper()}_H"
    return (
        f"#ifndef {guard}\n#define {guard}\n\n"
        "#include <stddef.h>\n#include <stdint.h>\n\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        f"extern const uint8_t {symbol}_data[];\n"
        f"extern const size_t {symbol}_size;\n\n"
        "#ifdef __cplusplus\n}\n#endif\n\n#endif\n"
    )


def render_source(symbol: str, header: str, data: bytes) -> str:
    lines = [f'#include "{header}"', "", f"const uint8_t {symbol}_data[] = {{"]
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    lines += ["};", "", f"const size_t {symbol}_size = sizeof({symbol}_data);", ""]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--symbol-source", action="append", default=[], type=Path)
    parser.add_argument("--range", action="append", default=[])
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--out-c", required=True, type=Path)
    parser.add_argument("--out-h", required=True, type=Path)
    args = parser.parse_args()
    if not IDENTIFIER.fullmatch(args.symbol):
        raise ValueError("symbol must be a valid C identifier")
    required = collect_codepoints(args.symbol_source, args.range)
    data = subset_font(args.font.read_bytes(), required)
    missing = required - font_codepoints(data)
    if missing:
        values = ", ".join(f"U+{value:04X}" for value in sorted(missing))
        raise RuntimeError(f"generated font misses required codepoints: {values}")
    args.out_h.write_text(render_header(args.symbol), encoding="utf-8")
    args.out_c.write_text(
        render_source(args.symbol, args.out_h.name, data), encoding="utf-8"
    )


if __name__ == "__main__":
    main()
