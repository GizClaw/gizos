"""Regenerate the Garden Guard Droid subsets from the pinned AOSP TTF."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess

from fontTools import subset
from fontTools.ttLib import TTFont

SOURCE_HASH = "05d71b179ef97b82cf1bb91cef290c600a510f77f39b4964359e3ef88378c79d"
PROJECT = Path(__file__).resolve().parents[1]
CANVAS = PROJECT / "app/src"
WEB = PROJECT / "targets/pkg_tar/web"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path, help="pinned DroidSansFallback.ttf")
    parser.add_argument("rasterizer", type=Path, help="compiled generate_han")
    args = parser.parse_args()
    if hashlib.sha256(args.font.read_bytes()).hexdigest() != SOURCE_HASH:
        parser.error("DroidSansFallback.ttf does not match the pinned AOSP source")
    bitmap = subprocess.check_output([
        args.rasterizer.resolve(), args.font, CANVAS / "h2_garden_guard_render.c",
    ])
    page = (WEB / "shell.html").read_text(encoding="utf-8")
    chars = {ord(ch) for ch in re.findall(r"[^\x00-\x7f]", page)}
    options = subset.Options(name_IDs=[0, 1, 2, 3, 4, 5, 6, 8, 9, 11, 13, 14])
    with TTFont(args.font, recalcTimestamp=False) as font:
        missing = chars - font.getBestCmap().keys()
        if missing:
            parser.error("missing Web glyphs: " + ", ".join(f"U+{cp:04X}" for cp in sorted(missing)))
        worker = subset.Subsetter(options=options)
        worker.populate(unicodes=sorted(chars))
        worker.subset(font)
        outlines = font["glyf"]
        for name in font.getGlyphOrder():
            glyph = outlines[name]
            if glyph.numberOfContours > 0:
                # The Android font predates modern browser validation. Retain
                # contour semantics and let fonttools re-encode the storage flags.
                for point, flag in enumerate(glyph.flags):
                    glyph.flags[point] = flag & 1
        font.flavor = "woff"
        font.save(WEB / "droid-sans-fallback.woff")
    (CANVAS / "h2_garden_guard_zh.inc").write_bytes(bitmap)
    glyph_count = len(re.findall(rb"^    \{0x[0-9a-f]+u,", bitmap, re.MULTILINE))
    print(f"Canvas glyphs: {glyph_count}; Web glyphs: {len(chars)}")


if __name__ == "__main__":
    main()
