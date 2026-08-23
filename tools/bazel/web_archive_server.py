#!/usr/bin/env python3
"""Safely extract and locally serve one Bazel-built Web tar archive."""

from __future__ import annotations

import argparse
from collections.abc import Iterable
from contextlib import contextmanager
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path, PurePosixPath
import tarfile
import tempfile
from typing import BinaryIO


MAX_ARCHIVE_MEMBERS = 4096
MAX_ARCHIVE_BYTES = 1024 * 1024 * 1024


def _safe_parts(name: str) -> tuple[str, ...]:
    if not name or "\\" in name:
        raise ValueError(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    parts = tuple(part for part in path.parts if part != ".")
    if path.is_absolute() or not parts or any(part == ".." for part in parts):
        raise ValueError(f"unsafe archive path: {name!r}")
    return parts


def _copy_exact(source: BinaryIO, destination: BinaryIO, expected: int) -> None:
    copied = 0
    while copied < expected:
        chunk = source.read(min(64 * 1024, expected - copied))
        if not chunk:
            break
        destination.write(chunk)
        copied += len(chunk)
    if copied != expected:
        raise ValueError("truncated archive member")


def extract_web_archive(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    seen: set[tuple[str, ...]] = set()
    total_bytes = 0
    with tarfile.open(archive, mode="r:*") as bundle:
        for member_count, member in enumerate(bundle, start=1):
            if member_count > MAX_ARCHIVE_MEMBERS:
                raise ValueError("Web archive contains too many entries")
            parts = _safe_parts(member.name)
            if parts in seen:
                raise ValueError(f"duplicate archive path: {member.name}")
            seen.add(parts)
            output = destination.joinpath(*parts)
            if member.isdir():
                output.mkdir(parents=True, exist_ok=True)
                continue
            if not member.isfile():
                raise ValueError(f"unsupported archive entry: {member.name}")
            total_bytes += member.size
            if total_bytes > MAX_ARCHIVE_BYTES:
                raise ValueError("Web archive is too large")
            output.parent.mkdir(parents=True, exist_ok=True)
            source = bundle.extractfile(member)
            if source is None:
                raise ValueError(f"missing archive payload: {member.name}")
            with source, output.open("wb") as target:
                _copy_exact(source, target, member.size)
    if not destination.joinpath("index.html").is_file():
        raise ValueError("Web archive root is missing index.html")


def read_header_policy(root: Path) -> list[tuple[str, list[tuple[str, str]]]]:
    source = root / "_headers"
    if not source.is_file():
        return []
    policies: list[tuple[str, list[tuple[str, str]]]] = []
    for line_number, raw in enumerate(
        source.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if not raw[0].isspace():
            pattern = raw.strip()
            if not pattern.startswith("/"):
                raise ValueError(f"invalid _headers route at line {line_number}")
            policies.append((pattern, []))
            continue
        if not policies or ":" not in raw:
            raise ValueError(f"invalid _headers value at line {line_number}")
        name, value = (part.strip() for part in raw.split(":", 1))
        if not name or not value or "\r" in value or "\n" in value:
            raise ValueError(f"invalid _headers value at line {line_number}")
        policies[-1][1].append((name, value))
    return policies


def _route_matches(pattern: str, request_path: str) -> bool:
    if pattern.endswith("*"):
        return request_path.startswith(pattern[:-1])
    return pattern == request_path


def headers_for_path(
    policies: Iterable[tuple[str, list[tuple[str, str]]]], request_path: str
) -> list[tuple[str, str]]:
    headers: list[tuple[str, str]] = []
    for pattern, values in policies:
        if _route_matches(pattern, request_path):
            headers.extend(values)
    return headers


def make_handler(
    root: Path, policies: list[tuple[str, list[tuple[str, str]]]]
) -> type[SimpleHTTPRequestHandler]:
    class WebArchiveHandler(SimpleHTTPRequestHandler):
        extensions_map = {
            **SimpleHTTPRequestHandler.extensions_map,
            ".data": "application/octet-stream",
            ".js": "text/javascript; charset=utf-8",
            ".wasm": "application/wasm",
        }

        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(root), **kwargs)

        def end_headers(self) -> None:
            request_path = self.path.split("?", 1)[0]
            for name, value in headers_for_path(policies, request_path):
                if name.lower() != "cache-control":
                    self.send_header(name, value)
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

    return WebArchiveHandler


@contextmanager
def prepared_archive(archive: Path):
    with tempfile.TemporaryDirectory(prefix="h2-web-archive-") as directory:
        root = Path(directory)
        extract_web_archive(archive, root)
        yield root


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Serve one Bazel-built Web tar archive"
    )
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if not args.archive.is_file():
        raise SystemExit(f"archive does not exist: {args.archive}")
    if args.port < 0 or args.port > 65535:
        raise SystemExit("port must be between 0 and 65535")
    with prepared_archive(args.archive.resolve()) as root:
        policies = read_header_policy(root)
        server = ThreadingHTTPServer(
            (args.host, args.port), make_handler(root, policies)
        )
        server.daemon_threads = True
        host, port = server.server_address[:2]
        display_host = f"[{host}]" if ":" in host else host
        print(f"Serving {args.archive} at http://{display_host}:{port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
