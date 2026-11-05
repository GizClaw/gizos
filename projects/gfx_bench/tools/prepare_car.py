#!/usr/bin/env python3
"""Convert the local car OBJ and livery PNG into a C data file.

The car model has no redistribution licence. Its inputs and the generated
output stay on this machine; see projects/gfx_bench/README.md.
"""

import argparse
import math
import struct
import zlib
from pathlib import Path

CAR_LENGTH = 4.0
SMOOTH_ANGLE_DEGREES = 55.0
LIVERY_SIZE = 256
REFLECTION_SIZE = 128


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    offset, palette, compressed = 8, None, bytearray()
    width = height = depth = color_type = interlace = None
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset : offset + 8])
        chunk = data[offset + 8 : offset + 8 + length]
        offset += length + 12
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif kind == b"PLTE":
            palette = chunk
        elif kind == b"IDAT":
            compressed += chunk
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if depth != 8 or interlace != 0 or channels is None:
        raise ValueError(f"{path}: unsupported PNG layout")
    raw = zlib.decompress(bytes(compressed))
    stride = width * channels
    previous = bytearray(stride)
    pixels, cursor = [], 0
    for _ in range(height):
        kind = raw[cursor]
        row = bytearray(raw[cursor + 1 : cursor + 1 + stride])
        cursor += stride + 1
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                row[i] = (row[i] + left) & 255
            elif kind == 2:
                row[i] = (row[i] + up) & 255
            elif kind == 3:
                row[i] = (row[i] + (left + up) // 2) & 255
            elif kind == 4:
                estimate = left + up - corner
                distances = (
                    abs(estimate - left),
                    abs(estimate - up),
                    abs(estimate - corner),
                )
                if distances[0] <= distances[1] and distances[0] <= distances[2]:
                    row[i] = (row[i] + left) & 255
                elif distances[1] <= distances[2]:
                    row[i] = (row[i] + up) & 255
                else:
                    row[i] = (row[i] + corner) & 255
        previous = row
        for x in range(width):
            sample = row[x * channels : x * channels + channels]
            if color_type == 3:
                pixels.append(tuple(palette[sample[0] * 3 : sample[0] * 3 + 3]))
            elif color_type in (0, 4):
                pixels.append((sample[0], sample[0], sample[0]))
            else:
                pixels.append((sample[0], sample[1], sample[2]))
    return width, height, pixels


def rgb565(red, green, blue):
    clamp = lambda value: max(0, min(255, int(value)))
    return (
        ((clamp(red) >> 3) << 11) | ((clamp(green) >> 2) << 5) | (clamp(blue) >> 3)
    )


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def normalized(value):
    length = math.sqrt(dot(value, value))
    return (0.0, 1.0, 0.0) if length < 1e-9 else tuple(c / length for c in value)


def read_obj(path):
    positions, uvs, normals = [], [], []
    groups = {"body": [], "wheels": [], "windows": []}
    object_name, material = "", ""
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "v":
            positions.append(tuple(float(v) for v in parts[1:4]))
        elif parts[0] == "vt":
            uvs.append((float(parts[1]), float(parts[2])))
        elif parts[0] == "vn":
            normals.append(tuple(float(v) for v in parts[1:4]))
        elif parts[0] == "o":
            object_name = parts[1]
        elif parts[0] == "usemtl":
            material = parts[1]
        elif parts[0] == "f":
            corners = [tuple(int(v) - 1 for v in c.split("/")) for c in parts[1:]]
            if "Window" in material:
                group = "windows"
            elif object_name.startswith("Nascar_wheel"):
                group = "wheels"
            else:
                group = "body"
            for i in range(1, len(corners) - 1):
                groups[group].append((corners[0], corners[i], corners[i + 1]))
    return positions, uvs, normals, groups


def build_mesh(path):
    positions, uvs, normals, groups = read_obj(path)
    # The export is mirrored with inward normals. Reflecting X and negating the
    # Y/Z normal components keeps the livery readable and normals outward.
    minimum = [min(p[i] for p in positions) for i in range(3)]
    maximum = [max(p[i] for p in positions) for i in range(3)]
    scale = CAR_LENGTH / (maximum[2] - minimum[2])
    middle = (
        (minimum[0] + maximum[0]) / 2,
        minimum[1],
        (minimum[2] + maximum[2]) / 2,
    )

    def place(p):
        return (
            -(p[0] - middle[0]) * scale,
            (p[1] - middle[1]) * scale,
            (p[2] - middle[2]) * scale,
        )

    def turn(n):
        return normalized((n[0], -n[1], -n[2]))

    vertices, lookup, indices, ranges = [], {}, [], {}
    cos_limit = math.cos(math.radians(SMOOTH_ANGLE_DEGREES))
    for name in ("body", "wheels", "windows"):
        faces = groups[name]
        shared = {}
        for face in faces:
            for corner in face:
                shared.setdefault(corner[0], []).append(turn(normals[corner[2]]))
        ranges[name] = (len(indices), len(faces))
        for face in faces:
            triangle = []
            for corner in face:
                hard = turn(normals[corner[2]])
                nearby = [n for n in shared[corner[0]] if dot(n, hard) >= cos_limit]
                total = [sum(n[i] for n in nearby) for i in range(3)]
                smooth = normalized(total)
                uv = uvs[corner[1]]
                record = (place(positions[corner[0]]), hard, smooth, uv)
                key = tuple(round(v, 5) for part in record for v in part)
                if key not in lookup:
                    lookup[key] = len(vertices)
                    vertices.append(record)
                triangle.append(lookup[key])
            indices.append(tuple(triangle))
    extent = [
        max(abs(v[0][i]) for v in vertices) if i != 1 else max(v[0][1] for v in vertices)
        for i in range(3)
    ]
    return vertices, indices, ranges, extent


def reflection_texture():
    """A studio light probe indexed by the view-space normal."""
    pixels = []
    for row in range(REFLECTION_SIZE):
        for column in range(REFLECTION_SIZE):
            nx = (column + 0.5) / REFLECTION_SIZE * 2 - 1
            ny = 1 - (row + 0.5) / REFLECTION_SIZE * 2
            length = nx * nx + ny * ny
            nz = math.sqrt(max(0.0, 1 - length))
            ry = 2 * nz * ny
            rx = 2 * nz * nx
            sky = max(0.0, ry)
            red = 28 + 120 * sky
            green = 34 + 140 * sky
            blue = 52 + 190 * sky
            if ry < 0:
                floor = min(1.0, -ry * 1.6)
                red, green, blue = 44 * (1 - floor) + 10 * floor, 40 * (
                    1 - floor
                ) + 9 * floor, 58 * (1 - floor) + 18 * floor
            box = max(0.0, 1 - (abs(rx + 0.25) / 0.34) ** 4)
            box *= max(0.0, 1 - (abs(ry - 0.68) / 0.22) ** 4)
            red, green, blue = red + 255 * box, green + 250 * box, blue + 240 * box
            cyan = max(0.0, 1 - (abs(ry - 0.1) / 0.035) ** 2) * max(0.0, -rx)
            pink = max(0.0, 1 - (abs(ry - 0.16) / 0.035) ** 2) * max(0.0, rx)
            red += 230 * pink + 20 * cyan
            green += 60 * pink + 210 * cyan
            blue += 190 * pink + 235 * cyan
            pixels.append(rgb565(red, green, blue))
    return pixels


def number(value):
    return f"{value:.6f}f"


def words(values):
    rows = []
    for start in range(0, len(values), 12):
        rows.append(
            "  " + ",".join(f"0x{v:04X}" for v in values[start : start + 12]) + ","
        )
    return "\n".join(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obj", required=True)
    parser.add_argument("--livery", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    vertices, indices, ranges, extent = build_mesh(args.obj)
    width, height, livery = read_png(args.livery)
    if (width, height) != (LIVERY_SIZE, LIVERY_SIZE):
        raise ValueError("the livery must be 256x256")
    out = ['#include "h2_car_model.h"', ""]
    out.append("static const h2_car_vertex_t vertices[] = {")
    for position, hard, smooth, uv in vertices:
        fields = [
            ",".join(number(v) for v in part) for part in (position, hard, smooth, uv)
        ]
        out.append("  {{%s},{%s},{%s},{%s}}," % tuple(fields))
    out += ["};", "", "static const uint16_t indices[] = {"]
    out += [f"  {a},{b},{c}," for a, b, c in indices]
    out += ["};", "", "static const uint16_t livery[] = {"]
    out.append(words([rgb565(*p) for p in livery]))
    out += ["};", "", "static const uint16_t reflection[] = {"]
    out.append(words(reflection_texture()))
    out += ["};", "", "const h2_car_model_t h2_car_model = {"]
    out.append("  .vertices = vertices,")
    out.append(f"  .vertex_count = {len(vertices)},")
    out.append("  .indices = indices,")
    out.append(f"  .triangle_count = {len(indices)},")
    for name in ("body", "wheels", "windows"):
        out.append(f"  .{name} = {{{ranges[name][0]}, {ranges[name][1]}}},")
    out.append("  .livery = livery,")
    out.append("  .reflection = reflection,")
    out.append(f"  .half_width = {number(extent[0])},")
    out.append(f"  .height = {number(extent[1])},")
    out.append(f"  .half_length = {number(extent[2])},")
    out += ["};", ""]
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(out))
    print(
        f"vertices={len(vertices)} triangles={len(indices)} ranges={ranges} "
        f"extent={[round(v, 3) for v in extent]} output={output}"
    )


if __name__ == "__main__":
    main()
