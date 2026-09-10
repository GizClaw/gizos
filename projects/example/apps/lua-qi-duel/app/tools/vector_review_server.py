#!/usr/bin/env python3
"""Loopback-only artwork review; persists a bounded, explicitly requested report."""
import hashlib
import http.server
import json
from pathlib import Path

APP = Path(__file__).resolve().parents[2]
PORT = 4189
REPORT = APP / 'validation/vector/artwork-review.json'


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(APP), **kwargs)

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def do_POST(self):
        if self.path != '/save-vector-report':
            self.send_error(404)
            return
        if self.headers.get('Origin') != f'http://127.0.0.1:{PORT}':
            self.send_error(403)
            return
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size < 2_000_000:
                raise ValueError('report size')
            report = json.loads(self.rfile.read(size))
            if report.get('status') != 'draft-not-accepted' or len(report['frames']) != 900:
                raise ValueError('not a complete draft comparison')
            assets = sorted((APP / 'assets/vector').glob('*.svg'))
            assets += [APP / 'assets/source/skill-icons-v1.png',
                       APP / 'assets/source/opponent-full.png',
                       APP / 'assets/generated/skill-styles.h2rs',
                       APP / 'assets/generated/skill-colors.h2rs',
                       APP / 'desktop-preview/vector-review.js']
            report['inputs_sha256'] = {
                str(p.relative_to(APP)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in assets
            }
            REPORT.parent.mkdir(parents=True, exist_ok=True)
            temporary = REPORT.with_suffix('.json.tmp')
            temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
            temporary.replace(REPORT)
        except (ValueError, KeyError, TypeError):
            self.send_error(400)
            return
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'saved': str(REPORT.relative_to(APP))}).encode())


if __name__ == '__main__':
    http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
