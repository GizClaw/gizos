"""Writes a Web page shell with one page layout (HTML/CSS) injected."""

import argparse
import pathlib

PLACEHOLDER = "<!-- H2_WEB_APP_LAYOUT -->"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--shell", type=pathlib.Path, required=True)
    parser.add_argument("--layout", type=pathlib.Path, required=True)
    parser.add_argument("--style", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    shell = args.shell.read_text(encoding="utf-8")
    if shell.count(PLACEHOLDER) != 1:
        raise SystemExit(f"{args.shell}: expected exactly one {PLACEHOLDER}")
    layout = args.layout.read_text(encoding="utf-8")
    if "{{{" in layout:
        raise SystemExit(f"{args.layout}: Emscripten template markers are not allowed")
    for style in args.style:
        content = style.read_text(encoding="utf-8")
        if "{{{" in content or "</style" in content.lower():
            raise SystemExit(f"{style}: expected CSS without HTML or template markers")
        layout += f"\n<style>\n{content}\n</style>\n"
    args.out.write_text(shell.replace(PLACEHOLDER, layout), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
