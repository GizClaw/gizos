"""Discover project-owned E2E apps without treating board entries as apps."""

import argparse
import json
from pathlib import Path


EXCLUDED_ENTRIES = frozenset(("peripherals", "perpheral", "chip", "loader"))


def discover_apps(repository):
    root = Path(repository) / "projects/e2e/apps"
    if not root.is_dir():
        raise ValueError("projects/e2e/apps is missing")
    return sorted(
        path.name
        for path in root.iterdir()
        if path.is_dir()
        and not path.name.startswith((".", "__"))
        and path.name not in EXCLUDED_ENTRIES
    )


def discover_entries(repository, kind):
    if kind not in ("single", "pair"):
        raise ValueError("E2E kind must be single or pair")
    root = Path(repository) / "projects/e2e/targets" / kind
    if not root.is_dir():
        raise ValueError(f"projects/e2e/targets/{kind} is missing")
    entries = []
    for build in sorted(root.glob("*/*/*/BUILD.bazel")):
        app, manufacturer, board = build.parent.relative_to(root).parts
        entries.append({
            "app": app,
            "manufacturer": manufacturer,
            "board": board,
            "path": build.parent.relative_to(Path(repository)).as_posix(),
        })
    return entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True, type=Path)
    parser.add_argument("--kind", choices=("single", "pair"))
    arguments = parser.parse_args()
    if arguments.kind is not None:
        entries = discover_entries(arguments.repository, arguments.kind)
        result = {"kind": arguments.kind, "entries": entries, "total": len(entries)}
    else:
        apps = discover_apps(arguments.repository)
        result = {"apps": apps, "total": len(apps)}
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
