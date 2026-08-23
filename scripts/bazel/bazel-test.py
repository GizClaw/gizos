#!/usr/bin/env python3
"""Test every compatible automatic Bazel target."""

import os
from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.bazel import cache_options  # noqa: E402


if __name__ == "__main__":
    try:
        args = [os.environ.get("BAZEL_BIN", "bazel"), "test"]
        config = os.environ.get("BAZEL_CONFIG", "")
        if config:
            args.append(f"--config={config}")
        args.extend(cache_options())
        args.append("//...")
        os.execvp(args[0], args)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
