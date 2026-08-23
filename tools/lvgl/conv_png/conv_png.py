#!/usr/bin/env python3
"""Convert a non-interlaced 8-bit RGBA PNG to an LVGL ARGB8888 C descriptor."""

from __future__ import annotations

import argparse
import re
import struct
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
SYMBOL_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def paeth(left: int, up: int, upper_left: int) -> int:
    estimate = left + up - upper_left
    distances = (abs(estimate - left), abs(estimate - up), abs(estimate - upper_left))
    return (left, up, upper_left)[distances.index(min(distances))]


def decode_rgba(path: Path) -> tuple[int, int, bytes]:
    png = path.read_bytes()
    if not png.startswith(PNG_SIGNATURE):
        raise ValueError(f"not a PNG file: {path}")
    position = len(PNG_SIGNATURE)
    width = height = 0
    compressed = bytearray()
    while position < len(png):
        length = struct.unpack_from(">I", png, position)[0]
        chunk_type = png[position + 4 : position + 8]
        data = png[position + 8 : position + 8 + length]
        position += length + 12
        if chunk_type == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", data
            )
            if (depth, color, compression, filtering, interlace) != (8, 6, 0, 0, 0):
                raise ValueError("input must be a non-interlaced 8-bit RGBA PNG")
        elif chunk_type == b"IDAT":
            compressed.extend(data)
        elif chunk_type == b"IEND":
            break
    if width == 0 or height == 0:
        raise ValueError("PNG has no valid IHDR")
    raw = zlib.decompress(compressed)
    row_bytes = width * 4
    expected = height * (row_bytes + 1)
    if len(raw) != expected:
        raise ValueError(f"unexpected decompressed size: {len(raw)} != {expected}")
    decoded = bytearray(height * row_bytes)
    previous = bytearray(row_bytes)
    cursor = 0
    for row_index in range(height):
        filter_type = raw[cursor]
        cursor += 1
        source = raw[cursor : cursor + row_bytes]
        cursor += row_bytes
        row = bytearray(row_bytes)
        for index, value in enumerate(source):
            left = row[index - 4] if index >= 4 else 0
            up = previous[index]
            upper_left = previous[index - 4] if index >= 4 else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                predictor = paeth(left, up, upper_left)
            else:
                raise ValueError(f"unsupported PNG filter: {filter_type}")
            row[index] = (value + predictor) & 0xFF
        decoded[row_index * row_bytes : (row_index + 1) * row_bytes] = row
        previous = row
    return width, height, bytes(decoded)


def emit_descriptor(input_path: Path, output_path: Path, symbol: str) -> None:
    if not SYMBOL_PATTERN.fullmatch(symbol):
        raise ValueError(f"invalid C symbol: {symbol}")
    if output_path.is_symlink():
        raise ValueError(f"output must not be a symlink: {output_path}")
    width, height, rgba = decode_rgba(input_path)
    bgra = bytearray()
    for index in range(0, len(rgba), 4):
        red, green, blue, alpha = rgba[index : index + 4]
        bgra.extend((blue, green, red, alpha))
    lines = []
    for index in range(0, len(bgra), 16):
        values = ", ".join(f"0x{value:02x}" for value in bgra[index : index + 16])
        lines.append(f"  {values},")
    source = f'''/* Generated from {input_path.name}; do not edit. */
#include "lvgl.h"

static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {symbol}_data[] = {{
{chr(10).join(lines)}
}};

const lv_image_dsc_t {symbol} = {{
  .header = {{
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_ARGB8888,
    .flags = 0,
    .w = {width},
    .h = {height},
    .stride = {width * 4},
  }},
  .data_size = sizeof({symbol}_data),
  .data = {symbol}_data,
}};
'''
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(source, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--symbol", required=True)
    args = parser.parse_args()
    emit_descriptor(args.input, args.output, args.symbol)


if __name__ == "__main__":
    main()
