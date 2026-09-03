#!/usr/bin/env python3
"""Build every Bazel target compatible with the selected config."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.bazel import cache_options  # noqa: E402


if __name__ == "__main__":
    try:
        args = [os.environ.get("BAZEL_BIN", "bazel")]
        config = os.environ.get("BAZEL_CONFIG", "")
        credentials = os.environ.get("H2LOADER_WIFI_CREDENTIALS", "")
        credentials_rc = None
        if config == "esp32s3" and credentials:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                prefix="h2-bazel-action-env-",
                suffix=".bazelrc",
                delete=False,
            ) as stream:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(
                    "build:esp --action_env=H2LOADER_WIFI_CREDENTIALS="
                    + shlex.quote(credentials)
                    + "\n"
                )
                credentials_rc = Path(stream.name)
            args.append(f"--bazelrc={credentials_rc}")
        args.append("build")
        if credentials_rc is not None:
            args.append("--noannounce_rc")
        if config:
            args.append(f"--config={config}")
        # How many cores each native firmware action reserves from Bazel's
        # local scheduler, which sets how many launchers build at once. Only
        # tunes scheduling, never the action key, so a job that lowers it still
        # shares cache entries with everything else. Left unset by default so
        # local builds keep the latency-optimized reservation.
        native_build_jobs = os.environ.get("BAZEL_NATIVE_BUILD_JOBS", "").strip()
        if native_build_jobs:
            if native_build_jobs not in {"1", "2", "4"}:
                raise ValueError(
                    "BAZEL_NATIVE_BUILD_JOBS must be 1, 2 or 4 when it is set"
                )
            args.append(f"--define=h2_native_build_jobs={native_build_jobs}")
        args.extend(cache_options())
        download_outputs = os.environ.get(
            "BAZEL_BUILD_REMOTE_DOWNLOAD_OUTPUTS", ""
        ).strip()
        if download_outputs not in {"", "minimal"}:
            raise ValueError(
                "BAZEL_BUILD_REMOTE_DOWNLOAD_OUTPUTS must be minimal when it is set"
            )
        if download_outputs:
            args.append(f"--remote_download_outputs={download_outputs}")
        args.append("//...")
        if credentials_rc is None:
            os.execvp(args[0], args)
        try:
            result = subprocess.run(args, check=False)
        finally:
            credentials_rc.unlink(missing_ok=True)
        raise SystemExit(result.returncode)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
