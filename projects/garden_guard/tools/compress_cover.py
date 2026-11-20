"""Add indexed row copies and Huffman coding to the original Garden Guard cover."""

import argparse
from collections import Counter
from heapq import heappop, heappush
from pathlib import Path
import re

ROWS_PER_GROUP = 16
MAX_RUN = 130
COPY = 256


def array_match(source, name):
    match = re.search(rf"\b{name}\[\] = \{{([^}}]+)\}};", source)
    if match is None:
        raise ValueError(f"Missing original {name} array")
    return match


def huffman(data):
    counts = Counter(data)
    heap = []
    nodes = []
    for value, count in sorted(counts.items()):
        heappush(heap, (count, value, -value - 1))
    serial = 512
    while len(heap) > 1:
        count_a, _, left = heappop(heap)
        count_b, _, right = heappop(heap)
        heappush(heap, (count_a + count_b, serial, len(nodes)))
        nodes.append((left, right))
        serial += 1
    root = heap[0][2]
    codes = {}

    def visit(node, bits, length):
        if node < 0:
            codes[-node - 1] = (bits, length)
            return
        left, right = nodes[node]
        visit(left, bits << 1, length + 1)
        visit(right, (bits << 1) | 1, length + 1)

    visit(root, 0, 0)
    return nodes, codes


def encode(data, rows, codes):
    encoded = bytearray()
    offsets = []
    bit_count = 0
    accumulator = 0
    pending = 0
    row_offsets = set(rows)
    for i, value in enumerate(data):
        if i in row_offsets:
            offsets.append(bit_count)
        bits, length = codes[value]
        accumulator = (accumulator << length) | bits
        pending += length
        bit_count += length
        while pending >= 8:
            pending -= 8
            encoded.append(accumulator >> pending)
            accumulator &= (1 << pending) - 1
    if pending:
        encoded.append(accumulator << (8 - pending))
    return encoded, offsets


def unpack_rows(data, offsets):
    rows = []
    for start, end in zip(offsets, offsets[1:] + [len(data)]):
        line = bytearray()
        while start < end:
            token = data[start]
            start += 1
            if token < 128:
                line.append(token)
            else:
                line.extend([data[start]] * (token - 128 + 3))
                start += 1
        rows.append(line)
    return rows


def tokenize(rows):
    """A group begins with a self-contained row. Copy runs reuse the prior row."""
    tokens, groups = [], []
    for y, row in enumerate(rows):
        previous = None if y % ROWS_PER_GROUP == 0 else rows[y - 1]
        if previous is None:
            groups.append(len(tokens))
        x = 0
        while x < len(row):
            limit = min(MAX_RUN, len(row) - x)
            run, copy = 1, 0
            while run < limit and row[x + run] == row[x]:
                run += 1
            while previous is not None and copy < limit and row[x + copy] == previous[x + copy]:
                copy += 1
            if copy >= 3 and copy >= run // 2:
                tokens.append(COPY + copy - 3)
                x += copy
            elif run >= 3:
                tokens.extend((128 + run - 3, row[x]))
                x += run
            else:
                tokens.append(row[x])
                x += 1
    return tokens, groups


def verify(encoded, offsets, nodes, rows):
    def symbol():
        nonlocal bit_offset
        node = len(nodes) - 1
        while node >= 0:
            bit = (encoded[bit_offset >> 3] >> (7 - (bit_offset & 7))) & 1
            bit_offset += 1
            node = nodes[node][bit]
        return -node - 1

    line = bytearray(len(rows[0]))
    for y, expected in enumerate(rows):
        first = y % ROWS_PER_GROUP == 0
        if first:
            bit_offset = offsets[y // ROWS_PER_GROUP]
        x = 0
        while x < len(line):
            token = symbol()
            if token < 128:
                line[x] = token
                x += 1
            elif token < COPY:
                length = token - 128 + 3
                line[x:x + length] = bytes([symbol()]) * length
                x += length
            else:
                if first:
                    raise ValueError(f"Unexpected cross-group copy in row {y}")
                x += token - COPY + 3
        if x != len(expected) or line != expected:
            raise ValueError(f"Bad decoded pixels in row {y}")


def print_array(stream, kind, name, values, columns, fmt):
    print(f"static const {kind} {name}[] = {{", file=stream)
    for start in range(0, len(values), columns):
        print("    " + ", ".join(fmt(value) for value in values[start:start + columns]) + ",", file=stream)
    print("};", file=stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="original row-RLE C include")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    row_table = array_match(source, "k_title_row_offsets")
    rows = [int(value) for value in re.findall(r"(\d+)u", row_table[1])]
    raw = array_match(source, "k_title_art")[1]
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]+)", raw))
    pixels = unpack_rows(data, rows)
    tokens, groups = tokenize(pixels)
    nodes, codes = huffman(tokens)
    encoded, offsets = encode(tokens, groups, codes)
    verify(encoded, offsets, nodes, pixels)
    with args.output.open("w", encoding="ascii", newline="\n") as stream:
        prefix = source[source.index("#define TITLE_ART_WIDTH"):source.rfind("\n", 0, row_table.start())].rstrip()
        print("/* Original cover, generated by tools/compress_cover.py. */", file=stream)
        print(prefix + "\n", file=stream)
        print(f"#define TITLE_ART_ROWS_PER_GROUP {ROWS_PER_GROUP}", file=stream)
        print_array(stream, "uint32_t", "k_title_bit_offsets", offsets, 9, lambda v: f"{v}u")
        print(file=stream)
        print("static const int16_t k_title_tree[][2] = {", file=stream)
        for start in range(0, len(nodes), 5):
            line = "    " + ", ".join(f"{{{a}, {b}}}" for a, b in nodes[start:start + 5]) + ","
            print(line, file=stream)
        print("};\n", file=stream)
        print_array(stream, "uint8_t", "k_title_art", encoded, 13, lambda v: f"0x{v:02x}")
    print(f"Cover bytes including index and decode table: {len(encoded) + 4 * (len(groups) + len(nodes))} (was {len(data) + 4 * len(rows)})")


if __name__ == "__main__":
    main()
