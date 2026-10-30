#!/usr/bin/env python3
"""Install the pinned public development-environment provider without resets."""

import fcntl
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
URL = "https://github.com/GizClaw/gizos-devenv.git"


def git(root, *args):
    return subprocess.check_output(
        ["git", "-c", "core.hooksPath=/dev/null", "-C", str(root), *args],
        text=True, env=dict(os.environ, GIT_TERMINAL_PROMPT="0"),
    ).strip()


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "esp"
    if target not in ("esp", "setup", "sources", "bk7258", "adb", "bk-arm-toolchain", "environment-test"):
        raise ValueError(f"unsupported DEVENV_TARGET: {target}")
    commit = (ROOT / "tools/bazel/native_versions/gizos_devenv_commit.txt").read_text().strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("gizos-devenv pin must be a full Git commit")
    destination = Path(os.environ.get("GIZOS_DEVENV_ROOT", ROOT.parent / "gizos-devenv")).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with (destination.parent / f".{destination.name}.setup.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not destination.exists():
            temporary = Path(tempfile.mkdtemp(prefix=f".{destination.name}.", dir=destination.parent))
            try:
                git(temporary, "init", "--quiet")
                git(temporary, "remote", "add", "origin", URL)
                git(temporary, "fetch", "--depth=1", "origin", commit)
                git(temporary, "checkout", "--quiet", "--detach", "FETCH_HEAD")
                temporary.rename(destination)
            finally:
                if temporary.exists():
                    shutil.rmtree(temporary)
        if git(destination, "rev-parse", "HEAD") != commit:
            raise ValueError(f"{destination} is not at {commit}; existing checkout was preserved")
        if git(destination, "status", "--porcelain=v1", "--untracked-files=no"):
            raise ValueError(f"{destination} has tracked changes; existing checkout was preserved")
    return subprocess.call(["make", "-C", str(destination), f"GIZOS_ROOT={ROOT}", target])


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"gizos-devenv: {error}", file=sys.stderr)
        sys.exit(1)
