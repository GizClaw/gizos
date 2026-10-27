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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True, type=Path)
    arguments = parser.parse_args()
    apps = discover_apps(arguments.repository)
    print(json.dumps({"apps": apps, "total": len(apps)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
