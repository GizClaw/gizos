"""Run production Web PAL Wasm in pinned Chromium with a loopback harness."""

import http.server
import http.client
import contextlib
import json
import os
from pathlib import Path
import secrets
import select
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time


@contextlib.contextmanager
def ice_server(binary):
    proc = subprocess.Popen([
        str(Path(binary).resolve()), "-listen=127.0.0.1:0", "-stun-listen=127.0.0.1:0",
        "-turn-listen=127.0.0.1:0", "-candidate-ip=127.0.0.1",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if not select.select([proc.stdout], [], [], 0.1)[0]:
                continue
            line = proc.stdout.readline().decode()
            if not line:
                raise RuntimeError("local ICE server exited before ready")
            print(line, end="")
            match = re.search(r"H2_WEBRTC_TEST_SERVER_READY .*stun=(127\.0\.0\.1:\d+) ", line)
            if match:
                http_match = re.search(r" http=127\.0\.0\.1:(\d+) ", line)
                turn_match = re.search(r" turn=(127\.0\.0\.1:\d+) ", line)
                if not http_match or not turn_match:
                    raise RuntimeError("missing local signaling or TURN endpoint")
                yield {"stun": "stun:" + match[1], "http_port": int(http_match[1]),
                       "turn": "turn:" + turn_match[1] + "?transport=udp"}
                return
        raise TimeoutError("local ICE server startup timeout")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        print(proc.stdout.read().decode(errors="replace"))
        proc.stdout.close()


def run_browser(endpoints, pion, relay=False):
    assert not relay or not pion
    artifacts = [Path(p).resolve() for arg in sys.argv[2:] for p in arg.split()]
    files = {p.name: p for p in artifacts}
    assert set(files) == {"webrtc_browser_test.js", "webrtc_browser_test.wasm"}
    browsers = list(Path(os.environ["TEST_SRCDIR"]).glob(
        "*h2_playwright_chromium*/chrome-*/headless_shell"))
    assert len(browsers) == 1, f"expected one pinned browser, found {browsers}"
    token = secrets.token_hex(16)
    result = []
    done = threading.Event()
    session_id = None
    completed = {"offers": 0, "closes": 0}

    def pion_request(method, path, body=None):
        connection = http.client.HTTPConnection("127.0.0.1", endpoints["http_port"], timeout=15)
        try:
            connection.request(method, path, body, {"Content-Type": "application/sdp"})
            response = connection.getresponse()
            payload = response.read(256 * 1024 + 1)
            if len(payload) > 256 * 1024:
                raise RuntimeError("Pion response exceeds test limit")
            if response.status not in (200, 204):
                raise RuntimeError(f"Pion {path} failed: {response.status}")
            return payload, response.getheader("X-H2-Session-ID")
        finally:
            connection.close()

    if relay:
        baseline, _ = pion_request("GET", "/turn-stats")
        baseline = json.loads(baseline)
        assert all(value == 0 for value in baseline.values()), baseline

    page = """<!doctype html><script>
const lines = [];
let reported = false;
function report(code) {
  if (reported) return;
  reported = true;
  fetch('/RESULT_TOKEN', {method:'POST', body:JSON.stringify({code, lines})});
}
var Module = {
  stunURL: STUN_URL,
  iceURL: ICE_URL,
  relay: RELAY_MODE,
  pion: PION_MODE,
  print: text => lines.push(String(text)),
  printErr: text => lines.push(String(text)),
  onAbort: reason => { lines.push(String(reason)); report(1); },
  onExit: code => report(code)
};
addEventListener('error', e => { lines.push(e.message); report(1); });
addEventListener('unhandledrejection', e => { lines.push(String(e.reason)); report(1); });
</script><script src='/webrtc_browser_test.js'></script>""".replace("RESULT_TOKEN", token).replace("STUN_URL", json.dumps(endpoints["stun"])).replace("ICE_URL", json.dumps(endpoints["turn" if relay else "stun"])).replace("RELAY_MODE", json.dumps(relay)).replace("PION_MODE", json.dumps(pion)).encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path == "/":
                data, mime = page, "text/html"
            elif self.path[1:] in files:
                path = files[self.path[1:]]
                data = path.read_bytes()
                mime = "application/wasm" if path.suffix == ".wasm" else "text/javascript"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            nonlocal session_id
            if pion and self.path in ("/pion/offer", "/pion/close"):
                if self.headers.get("Origin") != f"http://127.0.0.1:{self.server.server_port}":
                    self.send_error(403)
                    return
                try:
                    if self.path == "/pion/offer":
                        if session_id is not None:
                            raise RuntimeError("session already exists")
                        length = int(self.headers.get("Content-Length", "0"))
                        if not 0 < length <= 256 * 1024:
                            raise ValueError("invalid offer length")
                        payload, created_id = pion_request("POST", "/offer", self.rfile.read(length))
                        if not created_id or not re.fullmatch(r"[A-Za-z0-9-]+", created_id):
                            raise RuntimeError("invalid Pion session identifier")
                        session_id = created_id
                        completed["offers"] += 1
                    else:
                        if session_id is None:
                            raise RuntimeError("no session to close")
                        stats, _ = pion_request("GET", f"/session/{session_id}/channel-stats")
                        print("WEB_PION channel-stats=" + stats.decode())
                        counters = json.loads(stats)
                        if any(counters[key] != expected for key, expected in {
                            "created": 1, "opened": 1, "closed": 0, "current": 1, "max_current": 1
                        }.items()):
                            raise RuntimeError("unexpected Pion channel count")
                        pair, _ = pion_request("GET", f"/session/{session_id}/ice-pair")
                        pair = json.loads(pair)
                        if pair["local_protocol"] != "udp" or pair["remote_protocol"] != "udp":
                            raise RuntimeError("Pion did not select the expected UDP transport")
                        print("WEB_PION ice-pair=" + json.dumps(pair))
                        payload, _ = pion_request("POST", f"/session/{session_id}/close")
                        session_id = None
                        completed["closes"] += 1
                    self.send_response(200)
                    self.send_header("Content-Type", "application/sdp")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                except Exception as error:
                    print(f"WEB_PION proxy failure: {error}", flush=True)
                    self.send_error(502)
                return
            if self.path != f"/{token}":
                self.send_error(404)
                return
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 1024 * 1024:
                self.send_error(400)
                return
            result.append(json.loads(self.rfile.read(length)))
            self.send_response(204)
            self.end_headers()
            done.set()

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix="web-pal-browser-") as profile, tempfile.TemporaryFile() as log:
                proc = subprocess.Popen([
                    str(browsers[0]), "--headless", "--no-sandbox", "--disable-gpu",
                    "--autoplay-policy=no-user-gesture-required", "--no-first-run",
                    f"--user-data-dir={profile}", f"http://127.0.0.1:{server.server_port}/",
                ], stdout=log, stderr=log, start_new_session=True)
                try:
                    deadline = time.monotonic() + 90
                    while not done.wait(0.1):
                        if proc.poll() is not None:
                            raise RuntimeError(f"browser exited early: {proc.returncode}")
                        if time.monotonic() >= deadline:
                            raise TimeoutError("browser did not report terminal result")
                finally:
                    # Terminate the isolated browser's entire process group, including children.
                    try:
                        os.killpg(proc.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait(timeout=5)
                    log.seek(0)
                    print(log.read().decode(errors="replace"))
        finally:
            server.shutdown()
            thread.join(timeout=5)
            if session_id is not None:
                pion_request("POST", f"/session/{session_id}/close")
                session_id = None
    assert len(result) == 1, result
    print("\n".join(result[0]["lines"]))
    assert result[0]["code"] == 0, result
    assert result[0]["lines"].count("WEB_BROWSER_COMPLETE") == 1, result
    expected_backend = "pion" if pion else "browser"
    assert result[0]["lines"].count(f"WEB_BROWSER backend={expected_backend}") == 1, result
    # A second browser-to-browser run must never masquerade as Pion coverage.
    expected_sessions = 1 if pion else 0
    assert completed == {"offers": expected_sessions, "closes": expected_sessions}, completed
    assert result[0]["lines"].count(f"WEB_BROWSER relay={int(relay)}") == 1, result
    if relay:
        deadline = time.monotonic() + 5
        while True:
            stats, _ = pion_request("GET", "/turn-stats")
            stats = json.loads(stats)
            if stats["allocations_created"] > 0 and stats["allocations_deleted"] == stats["allocations_created"]:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(f"TURN allocation not released: {stats}")
            time.sleep(0.05)
        assert all(stats[key] > 0 for key in ("permissions_created", "relay_ingress", "relay_egress")), stats
        print("WEB_BROWSER turn-stats=" + json.dumps(stats))
    print(f"WEB_BROWSER backend={expected_backend} relay={int(relay)} cleanup=complete")


if __name__ == "__main__":
    with ice_server(sys.argv[1]) as endpoints:
        run_browser(endpoints, False)
        run_browser(endpoints, True)
        run_browser(endpoints, False, relay=True)
