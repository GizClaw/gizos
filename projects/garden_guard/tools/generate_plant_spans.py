"""Regenerate the AC791N plant bank from the original RGB565 renderer."""

from collections import defaultdict
import argparse
import lzma
from pathlib import Path
import subprocess


def generate(project, scratch, cc):
    renderer = project / "app/src/h2_garden_guard_render.c"
    capture = renderer.read_text()
    marker = "static void plot("
    capture = capture.replace(
        marker,
        "static void capture_pixel(int32_t x, int32_t y, uint16_t color, uint32_t alpha);\n\n" + marker,
        1,
    )
    pos = capture.index("  if (depth != NULL) {", capture.index(marker))
    capture = (
        capture[:pos]
        + "  (void)depth;\n"
        + "  capture_pixel(x, (int32_t)((row - canvas->pixels) / canvas->stride_pixels), color, alpha);\n"
        + "  return;\n"
        + capture[pos:]
    )
    scratch.mkdir(exist_ok=True, parents=True)
    (scratch / "capture.c").write_text(capture)
    repo = project.parent.parent
    generator = scratch / "generate_plant_spans"
    subprocess.run(
        [cc, "-O2", "-ffp-contract=off", "-ffunction-sections", "-fdata-sections",
         "-Wl,-dead_strip", "-I" + str(scratch), "-I" + str(project / "app/src"),
         "-I" + str(project / "app/include"), "-I" + str(repo / "libs/runtime/include"),
         "-I" + str(repo / "pal/include"), str(project / "tools/generate_plant_spans.c"),
         str(project / "app/src/h2_garden_guard_game.c"), "-o", str(generator)],
        check=True,
    )
    raw, index, sizes = (scratch / name for name in ("bank.bin", "plant_spans_index.inc", "sizes.txt"))
    subprocess.run([generator, raw, index, sizes], check=True)
    pack(raw, sizes, scratch)


def pack(raw_path, sizes_path, output):
    raw = raw_path.read_bytes()
    groups = defaultdict(bytearray)
    offset = 0
    for row in sizes_path.read_text().splitlines():
        lane, column, kind, _, size = map(int, row.split())
        groups[lane, column, kind].extend(raw[offset:offset + size])
        offset += size
    assert len(groups) == 90 and b"".join(groups.values()) == raw
    packed = bytearray()
    entries = []
    offset = 0
    for group in groups.values():
        block = lzma.compress(
            group,
            check=lzma.CHECK_CRC32,
            filters=[{"id": lzma.FILTER_LZMA2, "preset": 1, "dict_size": 131072}],
        )
        assert lzma.decompress(block) == group
        entries.append(f"  {{{len(packed)}u, {len(block)}u, {offset}u, {len(group)}u}},\n")
        packed.extend(block)
        offset += len(group)
    (output / "plant_spans.xzbank").write_bytes(packed)
    (output / "plant_spans_groups.inc").write_text(
        "static const struct plant_span_group plant_span_groups[] = {\n"
        + "".join(entries) + "};\n"
        + f"enum {{ PLANT_SPAN_BYTES={len(raw)}, PLANT_SPAN_GROUPS={len(groups)} }};\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.output.resolve(), args.cc)


if __name__ == "__main__":
    main()
