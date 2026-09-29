#!/usr/bin/env python3
"""Reject embedded compile outputs that name absolute system header paths."""

import argparse
import pathlib
import posixpath
import sys


def dependency_paths(text: str) -> list[str]:
    """Returns the prerequisites listed in a Make-style dependency file."""
    joined = text.replace("\\\n", " ")
    paths = []
    for line in joined.splitlines():
        _, separator, prerequisites = line.partition(": ")
        if not separator:
            continue
        token = ""
        index = 0
        while index < len(prerequisites):
            character = prerequisites[index]
            if character == "\\" and index + 1 < len(prerequisites) and prerequisites[index + 1] == " ":
                token += " "
                index += 2
                continue
            if character.isspace():
                if token:
                    paths.append(token)
                token = ""
            else:
                token += character
            index += 1
        if token:
            paths.append(token)
    return paths


def _is_under(path: str, directory: str) -> bool:
    return path == directory or path.startswith(directory + "/")


def check(builtin_directories: list[str], dependency_files: dict[str, str]) -> list[str]:
    """Returns every problem found in the toolchain directories and dependency files."""
    errors = []
    if not builtin_directories:
        errors.append("the toolchain declares no builtin include directories")
    relative_directories = []
    for directory in builtin_directories:
        if posixpath.isabs(directory):
            errors.append(f"builtin include directory is absolute: {directory}")
        else:
            relative_directories.append(posixpath.normpath(directory))
    for name, text in dependency_files.items():
        paths = dependency_paths(text)
        system_headers = 0
        for path in paths:
            if posixpath.isabs(path):
                errors.append(f"{name}: dependency is an absolute path: {path}")
                continue
            normalized = posixpath.normpath(path)
            if any(_is_under(normalized, directory) for directory in relative_directories):
                system_headers += 1
        if system_headers == 0:
            errors.append(f"{name}: no dependency lies under a builtin include directory")
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--builtin-include-directory", action="append", default=[])
    parser.add_argument("--dependency-file", action="append", default=[], type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args(argv)
    dependency_files = {
        str(path): path.read_text(encoding="utf-8")
        for path in arguments.dependency_file
    }
    errors = check(arguments.builtin_include_directory, dependency_files)
    if errors:
        print(
            "embedded toolchain names absolute system header paths; compile outputs would "
            "depend on the Bazel output base and break cache hits from other workspaces:",
            file=sys.stderr,
        )
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    lines = [f"builtin {directory}" for directory in arguments.builtin_include_directory]
    lines.extend(f"checked {name}" for name in dependency_files)
    arguments.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
