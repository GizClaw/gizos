"""Serve only the versioned public tone input; no GizClaw business endpoints."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import threading
from urllib.parse import urlsplit

PATH = "/gizclaw-e2e/playback-tone-v1.ogg"


def byte_range(value, size):
    if value is None:
        return 0, size - 1, 200
    match = re.fullmatch(r"bytes=([0-9]*)-([0-9]*)", value)
    if not match or not any(match.groups()):
        raise ValueError("invalid range")
    first, last = match.groups()
    if not first:
        suffix = int(last)
        if suffix == 0:
            raise ValueError("empty suffix")
        start, end = max(0, size - suffix), size - 1
    else:
        start, end = int(first), min(int(last), size - 1) if last else size - 1
    if start > end or start >= size:
        raise ValueError("unsatisfiable range")
    return start, end, 206


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self):
        if urlsplit(self.path).path != PATH:
            self.send_error(404)
            return
        payload = self.server.payload
        try:
            start, end, status = byte_range(self.headers.get("Range"), len(payload))
        except ValueError:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{len(payload)}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(status)
        self.send_header("Content-Type", "audio/ogg")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Expose-Headers", "Content-Length, Content-Range, Accept-Ranges")
        self.send_header("Content-Length", str(end - start + 1))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(payload)}")
        self.end_headers()
        if self.command == "GET":
            self.wfile.write(payload[start:end + 1])
        with self.server.receipt_lock:
            self.server.requests.append({"method": self.command, "status": status,
                                         "first": start, "last": end})
            if self.server.receipt:
                self.server.receipt.write_text(json.dumps({
                    "asset_sha256": hashlib.sha256(payload).hexdigest(),
                    "asset_bytes": len(payload), "requests": self.server.requests,
                }, indent=2) + "\n")

    do_GET = do_HEAD = reply

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Range")
        self.send_header("Content-Length", "0")
        self.end_headers()


def server(file, bind="127.0.0.1", port=0, receipt=None):
    payload = Path(file).read_bytes()
    if not payload.startswith(b"OggS"):
        raise ValueError("materialize the versioned Ogg fixture before serving")
    instance = ThreadingHTTPServer((bind, port), Handler)
    instance.payload, instance.receipt = payload, receipt
    instance.requests, instance.receipt_lock = [], threading.Lock()
    return instance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path, required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    with server(args.file, args.bind, args.port, args.receipt) as instance:
        print(json.dumps({"ready": True, "port": instance.server_port,
                          "path": PATH, "asset_bytes": len(instance.payload),
                          "asset_sha256": hashlib.sha256(instance.payload).hexdigest()}), flush=True)
        try:
            instance.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
