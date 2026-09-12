"""Writes an app_host shell with one page panel (HTML/CSS) injected."""

import argparse
import pathlib

PLACEHOLDER = "<!-- H2_WEB_APP_PANEL -->"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--shell", type=pathlib.Path, required=True)
    parser.add_argument("--panel", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    shell = args.shell.read_text(encoding="utf-8")
    if shell.count(PLACEHOLDER) != 1:
        raise SystemExit(f"{args.shell}: expected exactly one {PLACEHOLDER}")
    panel = args.panel.read_text(encoding="utf-8")
    if "{{{" in panel:
        raise SystemExit(f"{args.panel}: Emscripten template markers are not allowed")
    args.out.write_text(shell.replace(PLACEHOLDER, panel), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
