"""Losslessly pack approved H2VG contour frames for one screen or both screens."""

import argparse
from pathlib import Path
import re
import struct
import zlib


def unsigned_varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return result


def pack_frame(data):
    """Replace v1 absolute polygons with exact v2 delta polygons (opcode 14)."""
    if len(data) < 13 or data[:4] != b"H2VG" or data[8:12] != b"\0\0\1\0":
        raise ValueError("Expected a version-1 contour frame without gradients")
    result = bytearray(data[:10] + b"\2\0")
    cursor = 12
    while cursor < len(data):
        opcode = data[cursor]
        cursor += 1
        if opcode == 0:
            if cursor != len(data):
                raise ValueError("Trailing frame bytes")
            result.append(0)
            return bytes(result)
        if opcode == 4:
            result.append(4)
        elif opcode == 10:
            if cursor + 15 > len(data):
                raise ValueError("Truncated paint")
            result.append(10)
            result.extend(data[cursor:cursor + 15])
            cursor += 15
        elif opcode == 13:
            count, = struct.unpack_from("<H", data, cursor)
            cursor += 2
            if count < 3:
                raise ValueError("Invalid polygon")
            coordinates = struct.unpack_from("<" + "h" * (2 * count), data, cursor)
            cursor += count * 4
            unit = 4 if all(value % 4 == 0 for value in coordinates) else 1
            result.extend(struct.pack("<BHB", 14, count, unit))
            previous = [0, 0]
            for index, coordinate in enumerate(coordinates):
                value = coordinate // unit
                delta = value - previous[index % 2]
                previous[index % 2] = value
                result.extend(unsigned_varint(2 * delta if delta >= 0 else -2 * delta - 1))
        else:
            raise ValueError(f"Unsupported contour opcode {opcode}")
    raise ValueError("Missing frame terminator")


def unpack_frame(data):
    """Reference decoder used at build time to prove exact command preservation."""
    if data[:4] != b"H2VG" or data[8:12] != b"\0\0\2\0":
        raise ValueError("Expected a version-2 contour frame")
    result = bytearray(data[:10] + b"\1\0")
    cursor = 12
    while cursor < len(data):
        opcode = data[cursor]
        cursor += 1
        if opcode == 0:
            if cursor != len(data):
                raise ValueError("Trailing frame bytes")
            result.append(0)
            return bytes(result)
        if opcode == 4:
            result.append(4)
        elif opcode == 10:
            if cursor + 15 > len(data):
                raise ValueError("Truncated paint")
            result.append(10)
            result.extend(data[cursor:cursor + 15])
            cursor += 15
        elif opcode == 14:
            count, unit = struct.unpack_from("<HB", data, cursor)
            cursor += 3
            if count < 3 or unit not in (1, 4):
                raise ValueError("Invalid delta polygon")
            result.extend(struct.pack("<BH", 13, count))
            previous = [0, 0]
            for index in range(count * 2):
                value = 0
                for shift in (0, 7, 14):
                    byte = data[cursor]
                    cursor += 1
                    value |= (byte & 127) << shift
                    if byte < 128:
                        if value > 131070 or (shift and byte == 0):
                            raise ValueError("Invalid delta")
                        break
                else:
                    raise ValueError("Unterminated delta")
                delta = value // 2 if value % 2 == 0 else -(value // 2) - 1
                previous[index % 2] += delta
                result.extend(struct.pack("<h", previous[index % 2] * unit))
        else:
            raise ValueError(f"Unsupported contour opcode {opcode}")
    raise ValueError("Missing frame terminator")


def pack_components(bank, directory, screen):
    result = bytearray()
    lines = ["-- Generated lossless H2VG v2 directory; offsets address vector commands.", "return {"]
    frames = 0
    for line in directory.splitlines():
        name = re.match(r'\["([^"]+)"\]', line)
        if not name:
            continue
        if screen != "all" and name[1].endswith("-" + ("h106" if screen == "amoled" else "amoled")):
            continue
        entries = []
        for offset, size in re.findall(r"\{(\d+),(\d+)\}", line):
            offset, size = int(offset), int(size)
            if size < 5 or offset + size > len(bank):
                raise ValueError("Invalid source slice")
            source = bank[offset:offset + size]
            expected, = struct.unpack_from("<I", source)
            if not 13 <= expected <= 524288:
                raise ValueError("Invalid decoded frame length")
            decoder = zlib.decompressobj()
            raw = decoder.decompress(source[4:], expected + 1)
            if (len(raw) != expected or not decoder.eof or
                    decoder.unused_data or decoder.unconsumed_tail):
                raise ValueError("Invalid decoded frame length")
            encoded = pack_frame(raw)
            if unpack_frame(encoded) != raw:
                raise ValueError("Lossless command verification failed")
            compressed = struct.pack("<I", len(encoded)) + zlib.compress(encoded, 9)
            entries.append(f"{{{len(result)},{len(compressed)}}}")
            result.extend(compressed)
            frames += 1
        if not entries:
            raise ValueError("Missing component frames")
        lines.append(line.split("frames=", 1)[0] + "frames={" + ",".join(entries) + "}},")
    if not frames:
        raise ValueError("Empty component bank")
    lines.append("}")
    return bytes(result), "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bank", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--screen", choices=("all", "amoled", "h106"), default="all")
    parser.add_argument("--output-bank", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    bank, directory = pack_components(args.bank.read_bytes(), args.directory.read_text(), args.screen)
    args.output_bank.write_bytes(bank)
    args.output_directory.write_text(directory)


if __name__ == "__main__":
    main()
