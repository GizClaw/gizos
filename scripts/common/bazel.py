"""Shared Bazel command options for repository Make entrypoints."""

from __future__ import annotations

import os
from urllib.parse import urlsplit


def cache_options() -> list[str]:
    """Return the explicitly configured local and remote cache options."""
    options: list[str] = []
    disk_mode = os.environ.get("BAZEL_DISK_CACHE_MODE", "").strip()
    if disk_mode not in {"", "off"}:
        raise ValueError("BAZEL_DISK_CACHE_MODE must be off when it is set")
    url = os.environ.get("BAZEL_REMOTE_CACHE_URL", "").strip()
    if not url:
        return []
    if disk_mode == "off":
        options.append("--disk_cache=")

    parsed = urlsplit(url)
    path_parts = parsed.path.strip("/").split("/")
    if (
        parsed.scheme != "https"
        or parsed.netloc != "storage.googleapis.com"
        or parsed.query
        or parsed.fragment
        or len(path_parts) != 2
        or not path_parts[0]
        or path_parts[1] != "gizos"
    ):
        raise ValueError(
            "BAZEL_REMOTE_CACHE_URL must be "
            "https://storage.googleapis.com/<bucket>/gizos"
        )

    mode = os.environ.get("BAZEL_REMOTE_CACHE_MODE", "").strip()
    if mode not in {"read", "write"}:
        raise ValueError(
            "BAZEL_REMOTE_CACHE_MODE must be read or write when the remote "
            "cache is enabled"
        )

    return [
        *options,
        f"--remote_cache={url.rstrip('/')}",
        "--google_default_credentials",
        "--remote_timeout=60",
        "--remote_retries=5",
        f"--remote_upload_local_results={'true' if mode == 'write' else 'false'}",
    ]
