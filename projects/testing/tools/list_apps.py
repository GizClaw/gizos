"""Discover test suites and firmware entries by their testing category."""

import argparse
import json
from pathlib import Path


CATEGORIES = ("unit", "e2e", "integration", "benchmark")
ARTIFACT_RULES = {
    "h2_loader_tar_zlib": "h2loader_tar_zlib(",
    "h2_loader_zlib_tar": "h2loader_zlib_tar(",
}
EXCLUDED_ENTRIES = frozenset(("peripherals", "perpheral", "chip", "loader"))


def discover_apps(repository):
    root = Path(repository) / "projects/testing"
    if not root.is_dir():
        raise ValueError("projects/testing is missing")
    apps = set()
    for category in CATEGORIES:
        directory = root / category
        if not directory.is_dir():
            continue
        apps.update(
            path.name for path in directory.iterdir()
            if path.is_dir()
            and not path.name.startswith((".", "__"))
            and path.name not in EXCLUDED_ENTRIES
        )
    return sorted(apps)


def discover_entries(repository, kind, artifact_kind="h2_loader_tar_zlib"):
    if kind not in CATEGORIES:
        raise ValueError("Testing category must be " + ", ".join(CATEGORIES))
    if artifact_kind not in ARTIFACT_RULES:
        raise ValueError("Artifact kind must be " + ", ".join(ARTIFACT_RULES))
    root = Path(repository) / "projects/testing/targets" / artifact_kind / kind
    if not root.is_dir():
        raise ValueError(f"projects/testing/targets/{artifact_kind}/{kind} is missing")
    entries = []
    for build in sorted(root.rglob("BUILD.bazel")):
        parts = build.parent.relative_to(root).parts
        # Support exports/test helper packages without treating them as firmware.
        if ARTIFACT_RULES[artifact_kind] not in build.read_text():
            continue
        if len(parts) < 3:
            continue
        app, *role_parts, manufacturer, board = parts
        role = "/".join(role_parts)
        entry = {
            "app": app,
            "manufacturer": manufacturer,
            "board": board,
            "path": build.parent.relative_to(Path(repository)).as_posix(),
        }
        if role:
            entry["role"] = role
        entries.append(entry)
    return entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True, type=Path)
    parser.add_argument("--kind", choices=CATEGORIES)
    parser.add_argument("--artifact-kind", choices=ARTIFACT_RULES, default="h2_loader_tar_zlib")
    arguments = parser.parse_args()
    if arguments.kind is not None:
        entries = discover_entries(arguments.repository, arguments.kind, arguments.artifact_kind)
        result = {"kind": arguments.kind, "entries": entries, "total": len(entries)}
    else:
        apps = discover_apps(arguments.repository)
        result = {"apps": apps, "total": len(apps)}
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
