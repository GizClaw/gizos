#!/usr/bin/env python3
"""Run one explicitly selected manual Bazel test."""

from __future__ import annotations

import os
from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parent))
from bazel import cache_options  # noqa: E402


def command(label: str, options: list[str]) -> list[str]:
    """Build the Bazel argv for one explicitly selected manual test."""
    bazel_config = os.environ.get("BAZEL_CONFIG", "").strip()
    legacy_config = os.environ.get("H2_BAZEL_CONFIG", "").strip()
    if bazel_config and legacy_config and bazel_config != legacy_config:
        raise ValueError("BAZEL_CONFIG and H2_BAZEL_CONFIG must match")
    config = bazel_config or legacy_config
    args = [os.environ.get("BAZEL_BIN", "bazel"), "test"]
    if config:
        args.append(f"--config={config}")
    args.extend(cache_options())
    args.extend([
        "--cache_test_results=no",
        "--test_output=streamed",
        *options,
        label,
    ])
    return args


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit("usage: bazel_manual_test.py <label> [bazel-option ...]")
    try:
        args = command(sys.argv[1], sys.argv[2:])
        os.execvp(args[0], args)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error


if __name__ == "__main__":
    main()
