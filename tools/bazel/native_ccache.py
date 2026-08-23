"""Shared ccache configuration for native firmware build runners."""

from __future__ import annotations

import json
import os
import shlex
import stat
import subprocess
from pathlib import Path
from urllib.parse import quote, urlsplit


REMOTE_CCACHE_VERSION = "4.13.6"
RUNTIME_SCHEMA = "h2.native-ccache-runtime.v1"
REMOTE_FAMILY_BY_NAMESPACE = {
    "esp32p4": "esp",
    "esp32s3": "esp",
    "bk3633": "bk",
    "bk7258": "bk",
}


class NativeCcacheError(RuntimeError):
    """The native compiler cache configuration is invalid."""


def configure_environment(
    environment: dict[str, str],
    namespace: str,
    temporary_root: Path,
    runtime_locator: str,
) -> Path | None:
    """Validates and enables one target-specific ccache namespace."""
    locator_path = Path(runtime_locator)
    try:
        locator = json.loads(locator_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise NativeCcacheError(f"cannot read native ccache locator: {error}") from error
    if locator.get("schema") != "h2.native-locator.v1" or locator.get("kind") != "native-ccache-runtime":
        raise NativeCcacheError("invalid native ccache locator")
    if locator.get("enabled") is not True:
        return None
    paths = locator.get("paths")
    if not isinstance(paths, dict) or not isinstance(paths.get("manifest"), str):
        raise NativeCcacheError("native ccache locator has no runtime manifest")
    manifest_path = Path(paths["manifest"])
    try:
        runtime = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise NativeCcacheError(f"cannot read native ccache runtime manifest: {error}") from error
    if runtime.get("schema") != RUNTIME_SCHEMA:
        raise NativeCcacheError("invalid native ccache runtime manifest schema")
    root = manifest_path.parent.resolve()
    executable = _runtime_path(root, runtime, "ccache")
    cache_root = _runtime_path(root, runtime, "cache_root", require_exists=False)
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise NativeCcacheError(
            f"native ccache is unavailable or not executable: {executable}"
        )

    cache_directory = cache_root / namespace
    cache_directory.mkdir(parents=True, exist_ok=True)
    environment.update(
        {
            "CCACHE_BASEDIR": str(temporary_root),
            "CCACHE_COMPILERCHECK": "content",
            "CCACHE_COMPRESS": "1",
            "CCACHE_DIR": str(cache_directory),
            "CCACHE_MAXSIZE": "1GiB",
            "CCACHE_NAMESPACE": namespace,
            "CCACHE_NOHASHDIR": "1",
        }
    )
    result = subprocess.run(
        [str(executable), "--version"],
        check=False,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise NativeCcacheError(
            f"cannot execute native ccache: {result.stderr.strip()}"
        )
    remote_endpoint = _configure_remote_environment(
        environment,
        namespace,
        result.stdout,
        root,
        runtime,
    )
    remote_summary = f" remote={remote_endpoint}" if remote_endpoint else ""
    print(
        "Native compiler cache enabled: "
        f"namespace={namespace} directory={cache_directory}{remote_summary}"
    )
    return executable


def _configure_remote_environment(
    environment: dict[str, str],
    namespace: str,
    ccache_version_output: str,
    root: Path,
    runtime: dict[str, object],
) -> str | None:
    base_url = runtime.get("remote_base_url")
    if not base_url:
        return None
    if not isinstance(base_url, str):
        raise NativeCcacheError("native ccache remote_base_url must be a string")
    if namespace not in REMOTE_FAMILY_BY_NAMESPACE:
        raise NativeCcacheError(
            f"native ccache has no remote family for namespace: {namespace}"
        )
    expected_version = f"ccache version {REMOTE_CCACHE_VERSION}"
    if expected_version not in ccache_version_output.splitlines():
        raise NativeCcacheError(
            f"native remote ccache must be version {REMOTE_CCACHE_VERSION}"
        )

    endpoint = _remote_endpoint(base_url, REMOTE_FAMILY_BY_NAMESPACE[namespace])
    helper = _required_executable(
        str(_runtime_path(root, runtime, "storage_helper")),
        "native ccache storage helper",
    )
    token = _read_token_file(str(_runtime_path(root, runtime, "token_file")))
    environment.update(
        {
            "CCACHE_REMOTE_STORAGE": (
                f"{endpoint} helper={quote(str(helper), safe='/-._~')} "
                "request-timeout=30s data-timeout=10s "
                f"@layout=subdirs @bearer-token={quote(token, safe='-._~')}"
            ),
            "CCACHE_RESHARE": "1",
        }
    )
    return endpoint


def _runtime_path(
    root: Path,
    runtime: dict[str, object],
    name: str,
    *,
    require_exists: bool = True,
) -> Path:
    value = runtime.get(name)
    if not isinstance(value, str) or not value or Path(value).is_absolute():
        raise NativeCcacheError(f"native ccache {name} must be a relative path")
    path = (root / value).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise NativeCcacheError(f"native ccache {name} escapes the runtime root") from error
    if require_exists and not path.exists():
        raise NativeCcacheError(f"native ccache {name} is unavailable: {path}")
    return path


def _remote_endpoint(base_url: str, family: str) -> str:
    parsed = urlsplit(base_url)
    parts = [part for part in parsed.path.split("/") if part]
    if (
        parsed.scheme != "https"
        or parsed.netloc != "storage.googleapis.com"
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
        or len(parts) != 2
        or not parts[0]
        or parts[1] != "ccache"
    ):
        raise NativeCcacheError(
            "native ccache remote_base_url must be "
            "https://storage.googleapis.com/<bucket>/ccache"
        )
    return f"https://storage.googleapis.com/{parts[0]}/ccache/{family}"


def _required_executable(value: str, name: str) -> Path:
    executable = Path(value)
    if not executable.is_absolute():
        raise NativeCcacheError(f"{name} must be an absolute path: {executable}")
    if (
        executable.is_symlink()
        or not executable.is_file()
        or not os.access(executable, os.X_OK)
    ):
        raise NativeCcacheError(f"{name} is unavailable or not executable: {executable}")
    return executable


def _read_token_file(value: str) -> str:
    token_file = Path(value)
    if not token_file.is_absolute():
        raise NativeCcacheError(
            f"native ccache token file must be an absolute path: {token_file}"
        )
    try:
        metadata = token_file.lstat()
    except OSError as error:
        raise NativeCcacheError(
            f"native ccache token file is unavailable: {token_file}"
        ) from error
    if not stat.S_ISREG(metadata.st_mode) or token_file.is_symlink():
        raise NativeCcacheError(
            f"native ccache token file must be a regular file: {token_file}"
        )
    if stat.S_IMODE(metadata.st_mode) != 0o600:
        raise NativeCcacheError(
            f"native ccache token file must have mode 0600: {token_file}"
        )
    if hasattr(os, "geteuid") and metadata.st_uid != os.geteuid():
        raise NativeCcacheError(
            f"native ccache token file must be owned by the current user: {token_file}"
        )
    if metadata.st_size > 16 * 1024:
        raise NativeCcacheError("native ccache token file is unexpectedly large")
    token = token_file.read_text(encoding="utf-8").strip()
    if not token or any(character.isspace() for character in token):
        raise NativeCcacheError("native ccache token file has an invalid token")
    return token


def create_wrapped_toolchain(
    toolchain: Path,
    wrapper: Path,
    ccache: Path,
) -> Path:
    """Creates a toolchain bin directory with GCC compilation wrappers."""
    wrapper.mkdir()
    compiler_names = {
        "arm-none-eabi-c++",
        "arm-none-eabi-g++",
        "arm-none-eabi-gcc",
    }
    for source in toolchain.iterdir():
        destination = wrapper / source.name
        if source.name not in compiler_names:
            destination.symlink_to(source, target_is_directory=source.is_dir())
            continue
        destination.write_text(
            "#!/bin/sh\nexec "
            + shlex.quote(str(ccache))
            + " "
            + shlex.quote(str(source))
            + ' "$@"\n',
            encoding="utf-8",
        )
        destination.chmod(0o755)
    if not wrapper.joinpath("arm-none-eabi-gcc").is_file():
        raise NativeCcacheError(
            f"wrapped toolchain has no arm-none-eabi-gcc: {toolchain}"
        )
    return wrapper
