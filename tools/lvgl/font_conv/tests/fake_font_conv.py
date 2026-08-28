"""Deterministic fake for the font converter runner tests."""

import argparse
from pathlib import Path
import sys


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", required=True)
    parser.add_argument("--symbols", default="")
    parser.add_argument("--range", dest="ranges", action="append", default=[])
    parser.add_argument("--size", required=True)
    parser.add_argument("--bpp", required=True)
    parser.add_argument("--format", required=True)
    parser.add_argument("--lv-font-name", required=True)
    parser.add_argument("--lv-include", required=True)
    parser.add_argument("--no-compress", action="store_true")
    parser.add_argument("--no-prefilter", action="store_true")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if Path(args.font).name == "fail.ttf":
        sys.exit(7)
    if Path(args.font).name == "empty.ttf":
        return
    Path(args.output).write_text(
        "/* symbols=%s ranges=%s size=%s bpp=%s include=%s */\n"
        "const lv_font_t %s = {0};\n"
        % (
            ",".join("U+%04X" % ord(character) for character in args.symbols),
            ",".join(args.ranges),
            args.size,
            args.bpp,
            args.lv_include,
            args.lv_font_name,
        ),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
