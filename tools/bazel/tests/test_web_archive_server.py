from __future__ import annotations

from io import BytesIO
from pathlib import Path
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.parse import quote
from urllib.request import Request, urlopen

from tools.bazel.web_archive_server import (
    extract_web_archive,
    make_handler,
    read_header_policy,
)
from http.server import ThreadingHTTPServer


def write_archive(path: Path, entries: dict[str, bytes]) -> None:
    with tarfile.open(path, "w") as archive:
        for name, payload in entries.items():
            info = tarfile.TarInfo(name)
            info.size = len(payload)
            archive.addfile(info, BytesIO(payload))


class WebArchiveServerTest(unittest.TestCase):
    def test_rejects_member_overflow_without_preloading_headers(self) -> None:
        class StreamingArchive:
            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def __iter__(self):
                for index in range(3):
                    info = tarfile.TarInfo(
                        "index.html" if index == 0 else f"entry-{index}"
                    )
                    info.size = 0
                    yield info

            def getmembers(self):
                raise AssertionError("archive headers must not be preloaded")

            def extractfile(self, _member):
                return BytesIO()

        with tempfile.TemporaryDirectory() as directory:
            with patch(
                "tools.bazel.web_archive_server.MAX_ARCHIVE_MEMBERS", 2
            ), patch(
                "tools.bazel.web_archive_server.tarfile.open",
                return_value=StreamingArchive(),
            ):
                with self.assertRaisesRegex(ValueError, "too many entries"):
                    extract_web_archive(
                        Path(directory) / "unused.tar",
                        Path(directory) / "output",
                    )

    def test_extracts_and_serves_headers_and_wasm_mime(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "web.tar"
            output = root / "output"
            output.mkdir()
            write_archive(
                archive,
                {
                    "index.html": b"<canvas></canvas>",
                    "index.wasm": b"\x00asm",
                    "_headers": (
                        b"/*\n"
                        b"  Permissions-Policy: serial=(self)\n"
                        b"  X-Content-Type-Options: nosniff\n"
                    ),
                },
            )
            extract_web_archive(archive, output)
            policies = read_header_policy(output)
            server = ThreadingHTTPServer(
                ("127.0.0.1", 0), make_handler(output, policies)
            )
            server.daemon_threads = True
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                port = server.server_address[1]
                with urlopen(f"http://127.0.0.1:{port}/index.wasm") as response:
                    self.assertEqual(response.read(), b"\x00asm")
                    self.assertEqual(response.headers["Content-Type"],
                                     "application/wasm")
                    self.assertEqual(response.headers["Permissions-Policy"],
                                     "serial=(self)")
                    self.assertEqual(response.headers["X-Content-Type-Options"],
                                     "nosniff")
                    self.assertEqual(response.headers["Cache-Control"],
                                     "no-store")
            finally:
                server.shutdown()
                server.server_close()
                thread.join()

    def test_rejects_traversal_and_links(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "unsafe.tar"
            write_archive(archive, {"../index.html": b"unsafe"})
            with self.assertRaisesRegex(ValueError, "unsafe archive path"):
                extract_web_archive(archive, root / "output")

            link_archive = root / "link.tar"
            with tarfile.open(link_archive, "w") as bundle:
                info = tarfile.TarInfo("index.html")
                info.type = tarfile.SYMTYPE
                info.linkname = "/etc/passwd"
                bundle.addfile(info)
            with self.assertRaisesRegex(ValueError, "unsupported archive entry"):
                extract_web_archive(link_archive, root / "links")

    def test_opt_in_proxy_forwards_allowed_origin(self) -> None:
        from http.server import BaseHTTPRequestHandler

        received: dict[str, object] = {}

        class UpstreamHandler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                length = int(self.headers["Content-Length"])
                received["path"] = self.path
                received["body"] = self.rfile.read(length)
                received["authorization"] = self.headers["Authorization"]
                received["cookie"] = self.headers.get("Cookie")
                self.send_response(201)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"ok":true}')

            def log_message(self, _format, *_args) -> None:
                pass

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "index.html").write_text("ok", encoding="utf-8")
            upstream = ThreadingHTTPServer(("127.0.0.1", 0), UpstreamHandler)
            upstream.daemon_threads = True
            upstream_thread = threading.Thread(target=upstream.serve_forever)
            upstream_thread.start()
            upstream_origin = f"http://127.0.0.1:{upstream.server_address[1]}"
            server = ThreadingHTTPServer(
                ("127.0.0.1", 0),
                make_handler(root, [], frozenset({upstream_origin})),
            )
            server.daemon_threads = True
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                target = quote(f"{upstream_origin}/offer", safe="")
                request = Request(
                    f"http://127.0.0.1:{server.server_address[1]}"
                    f"/_h2/http-proxy?url={target}",
                    data=b"offer",
                    headers={
                        "Authorization": "Bearer test",
                        "Cookie": "local=secret",
                    },
                    method="POST",
                )
                with urlopen(request) as response:
                    self.assertEqual(response.status, 201)
                    self.assertEqual(response.read(), b'{"ok":true}')
                self.assertEqual(received["path"], "/offer")
                self.assertEqual(received["body"], b"offer")
                self.assertEqual(received["authorization"], "Bearer test")
                self.assertIsNone(received["cookie"])
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
                upstream.shutdown()
                upstream.server_close()
                upstream_thread.join()


if __name__ == "__main__":
    unittest.main()
