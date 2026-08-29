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
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlparse
from urllib.request import Request, urlopen


MAX_ARCHIVE_MEMBERS = 4096
MAX_ARCHIVE_BYTES = 1024 * 1024 * 1024
MAX_PROXY_BODY_BYTES = 64 * 1024 * 1024


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
    root: Path,
    policies: list[tuple[str, list[tuple[str, str]]]],
    proxy_origins: frozenset[str] = frozenset(),
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

        def _proxy_request(self) -> bool:
            parsed_request = urlparse(self.path)
            if parsed_request.path != "/_h2/http-proxy":
                return False
            values = parse_qs(parsed_request.query, keep_blank_values=True)
            targets = values.get("url", [])
            if len(targets) != 1:
                self.send_error(400, "one url query parameter is required")
                return True
            target = targets[0]
            parsed_target = urlparse(target)
            origin = f"{parsed_target.scheme}://{parsed_target.netloc}"
            if (
                parsed_target.scheme not in {"http", "https"}
                or not parsed_target.netloc
                or parsed_target.username is not None
                or parsed_target.fragment
                or origin not in proxy_origins
            ):
                self.send_error(403, "proxy target origin is not allowed")
                return True
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                self.send_error(400, "invalid Content-Length")
                return True
            if length < 0 or length > MAX_PROXY_BODY_BYTES:
                self.send_error(413, "proxy request body is too large")
                return True
            body = self.rfile.read(length) if length else None
            headers = {
                name: value
                for name, value in self.headers.items()
                if name.lower()
                not in {
                    "connection",
                    "content-length",
                    "cookie",
                    "host",
                    "origin",
                    "referer",
                }
                and not name.lower().startswith("sec-")
            }
            request = Request(
                target,
                data=body,
                headers=headers,
                method=self.command,
            )
            try:
                response = urlopen(request, timeout=60)
            except HTTPError as error:
                response = error
            except URLError as error:
                self.send_error(502, f"upstream request failed: {error.reason}")
                return True
            with response:
                payload = response.read(MAX_PROXY_BODY_BYTES + 1)
                if len(payload) > MAX_PROXY_BODY_BYTES:
                    self.send_error(502, "proxy response body is too large")
                    return True
                self.send_response(response.status)
                for name, value in response.headers.items():
                    if name.lower() not in {
                        "connection",
                        "content-length",
                        "transfer-encoding",
                    }:
                        self.send_header(name, value)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(payload)
            return True

        def do_GET(self) -> None:
            if not self._proxy_request():
                super().do_GET()

        def do_HEAD(self) -> None:
            if not self._proxy_request():
                super().do_HEAD()

        def do_POST(self) -> None:
            if not self._proxy_request():
                self.send_error(405)

        def do_PUT(self) -> None:
            if not self._proxy_request():
                self.send_error(405)

        def do_PATCH(self) -> None:
            if not self._proxy_request():
                self.send_error(405)

        def do_DELETE(self) -> None:
            if not self._proxy_request():
                self.send_error(405)

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
    parser.add_argument(
        "--http-proxy-origin",
        action="append",
        default=[],
        help="allow the opt-in local HTTP proxy to reach this exact origin",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if not args.archive.is_file():
        raise SystemExit(f"archive does not exist: {args.archive}")
    if args.port < 0 or args.port > 65535:
        raise SystemExit("port must be between 0 and 65535")
    with prepared_archive(args.archive.resolve()) as root:
        policies = read_header_policy(root)
        proxy_origins = frozenset(args.http_proxy_origin)
        server = ThreadingHTTPServer(
            (args.host, args.port), make_handler(root, policies, proxy_origins)
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
