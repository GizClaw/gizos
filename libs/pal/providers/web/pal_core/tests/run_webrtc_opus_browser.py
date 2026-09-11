"""Run the Opus vtable Track over real browser WebRTC against the Pion echo."""

import http.client
import http.server
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_webrtc_browser import ice_server  # noqa: E402  pylint: disable=wrong-import-position


def main():
    files = {p.name: p for arg in sys.argv[2:] for item in arg.split()
             for p in [Path(item).resolve()]}
    assert set(files) == {"webrtc_opus_test.js", "webrtc_opus_test.wasm"}, files
    custom = os.environ.get("H2_WEB_TEST_BROWSER")
    browsers = [Path(custom)] if custom else list(
        Path(os.environ["TEST_SRCDIR"]).glob(
            "*h2_playwright_chromium*/chrome-*/headless_shell"))
    assert len(browsers) == 1, browsers
    token = secrets.token_hex(16)
    result = []
    done = threading.Event()
    with ice_server(sys.argv[1]) as endpoints:
        page = ("""<!doctype html><script>
const lines = [];
let reported = false;
function report(code) {
  if (reported) return;
  reported = true;
  fetch('/RESULT', {method: 'POST', body: JSON.stringify({code, lines})});
}
var Module = {iceURL: ICE_URL,
  print: text => { lines.push(text); console.log(text); },
  printErr: text => { lines.push(text); console.error(text); },
  onExit: report, onAbort: text => { lines.push(String(text)); report(1); }};
</script><script src='/webrtc_opus_test.js'></script>"""
                .replace("RESULT", token)
                .replace("ICE_URL", json.dumps(endpoints["stun"])).encode())

        class Handler(http.server.BaseHTTPRequestHandler):
            def _send(self, status, payload, mime):
                self.send_response(status)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self):
                if self.path == "/":
                    self._send(200, page, "text/html")
                elif self.path[1:] in files:
                    mime = ("application/wasm" if self.path.endswith(".wasm")
                            else "text/javascript")
                    self._send(200, files[self.path[1:]].read_bytes(), mime)
                else:
                    self.send_error(404)

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                if self.path == "/" + token:
                    result.append(json.loads(body))
                    self._send(204, b"", "text/plain")
                    done.set()
                elif self.path == "/pion/offer":
                    connection = http.client.HTTPConnection(
                        "127.0.0.1", endpoints["http_port"], timeout=15)
                    connection.request("POST", "/offer", body,
                                       {"Content-Type": "application/sdp"})
                    response = connection.getresponse()
                    payload = response.read()
                    connection.close()
                    self._send(response.status, payload, "application/sdp")
                else:
                    self.send_error(404)

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        with tempfile.TemporaryDirectory(dir="/tmp" if custom else None) as directory:
            log_path = Path(directory) / "browser.log"
            with log_path.open("w+") as log:
                process = subprocess.Popen([
                    str(browsers[0]), "--headless", "--no-sandbox",
                    "--autoplay-policy=no-user-gesture-required",
                    "--enable-logging=stderr", "--v=0",
                    f"--user-data-dir={directory}/profile",
                    f"http://127.0.0.1:{server.server_port}/",
                ], stdout=log, stderr=log)
                try:
                    assert done.wait(60), "Opus Track browser test timed out"
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                    if not result or result[0]["code"] != 0:
                        log.seek(0)
                        print(log.read()[-12000:])
        server.shutdown()
    print(json.dumps(result))
    assert result and result[0]["code"] == 0, result
    assert any("WEB_OPUS unset=silent close=released PASS" in line
               for line in result[0]["lines"]), result


if __name__ == "__main__":
    main()
