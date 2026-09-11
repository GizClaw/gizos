"""Drive Web PAL netif, HTTP and filesystem scenarios in pinned Chromium.

The harness talks to headless Chromium over the DevTools pipe to switch the
page offline and back, reload it and open a second tab. The Wasm page reports each step as a "STEP {json}" console line.
"""

import http.server
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time

STREAM_BYTES = 262144


class Cdp:
    """Minimal Chrome DevTools Protocol client over --remote-debugging-pipe.

    A reader thread routes command replies to their callers and turns page
    console lines of the form "STEP {json}" into step reports, so reports
    still arrive while the page is offline.
    """

    def __init__(self, write_fd, read_fd, steps):
        self._write = os.fdopen(write_fd, "wb", buffering=0)
        self._read = os.fdopen(read_fd, "rb", buffering=0)
        self._steps = steps
        self._next_id = 0
        self._replies = {}
        self._condition = threading.Condition()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buffer = b""
        while True:
            chunk = self._read.read(65536)
            if not chunk:
                with self._condition:
                    self._replies[None] = True
                    self._condition.notify_all()
                return
            buffer += chunk
            while b"\0" in buffer:
                raw, buffer = buffer.split(b"\0", 1)
                message = json.loads(raw)
                if "id" in message:
                    with self._condition:
                        self._replies[message["id"]] = message
                        self._condition.notify_all()
                elif message.get("method") == "Runtime.consoleAPICalled":
                    args = message["params"].get("args") or [{}]
                    text = str(args[0].get("value", ""))
                    if text.startswith("STEP "):
                        self._steps.put(json.loads(text[5:]))

    def send(self, method, params=None, session=None):
        with self._condition:
            self._next_id += 1
            message_id = self._next_id
        message = {"id": message_id, "method": method, "params": params or {}}
        if session:
            message["sessionId"] = session
        self._write.write(json.dumps(message).encode() + b"\0")
        with self._condition:
            if not self._condition.wait_for(
                    lambda: message_id in self._replies or
                    None in self._replies, timeout=30):
                raise RuntimeError(f"{method} timed out")
            if message_id not in self._replies:
                raise RuntimeError("browser closed the DevTools pipe")
            reply = self._replies.pop(message_id)
        if "error" in reply:
            raise RuntimeError(f"{method} failed: {reply['error']}")
        return reply.get("result", {})


class QuietServer(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        # Aborted fetches (timeout, cancel, overflow) reset their connections.
        if not isinstance(sys.exc_info()[1], ConnectionError):
            super().handle_error(request, client_address)


def serve(handler_class):
    server = QuietServer(("127.0.0.1", 0), handler_class)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    files = {p.name: p for arg in sys.argv[1:] for item in arg.split()
             for p in [Path(item).resolve()]}
    assert set(files) == {"browser_platform_test.js",
                          "browser_platform_test.wasm",
                          "high_bframes_aac.mp4"}, files
    # H2_WEB_TEST_BROWSER selects an installed browser such as Google Chrome,
    # which ships the H.264/AAC codecs the pinned open-source Chromium lacks.
    custom = os.environ.get("H2_WEB_TEST_BROWSER")
    browsers = [Path(custom)] if custom else list(
        Path(os.environ["TEST_SRCDIR"]).glob(
            "*h2_playwright_chromium*/chrome-*/headless_shell"))
    assert len(browsers) == 1, browsers
    steps = queue.Queue()
    page = b"""<!doctype html><canvas id=canvas width=240 height=240></canvas>
<script>
var Module = {print: t => console.log(t), printErr: t => console.error(t),
              canvas: document.getElementById('canvas')};
</script><script src='/browser_platform_test.js'></script>"""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        cross_origin = False

        def _send(self, status, payload, mime="text/plain", cors=True,
                  extra=None):
            self.send_response(status)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(payload)))
            if cors:
                self.send_header("Access-Control-Allow-Origin", "*")
            for name, value in (extra or {}).items():
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(payload)

        def do_OPTIONS(self):
            self._send(204, b"", extra={
                "Access-Control-Allow-Methods": "GET, POST",
                "Access-Control-Allow-Headers": "content-type, x-h2-test",
                "Access-Control-Max-Age": "60",
            })

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/":
                self._send(200, page, "text/html")
            elif path[1:] in files:
                mime = ("application/wasm" if path.endswith(".wasm")
                        else "text/javascript")
                self._send(200, files[path[1:]].read_bytes(), mime)
            elif path == "/media/high_bframes_aac.mp4":
                self._send(200, files["high_bframes_aac.mp4"].read_bytes(),
                           "video/mp4")
            elif path == "/http/ok":
                self._send(200, b"ok")
            elif path == "/http/status/404":
                self._send(404, b"missing")
            elif path == "/http/nocors":
                self._send(200, b"secret", cors=False)
            elif path == "/http/slow":
                time.sleep(3)
                try:
                    self._send(200, b"late")
                except (BrokenPipeError, ConnectionResetError):
                    pass  # The client aborted, as the test intends.
            elif path == "/http/stream":
                self.send_response(200)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(STREAM_BYTES))
                self.end_headers()
                block = bytes(range(256)) * 16
                try:
                    for _ in range(STREAM_BYTES // len(block)):
                        self.wfile.write(block)
                        self.wfile.flush()
                        time.sleep(0.002)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # The overflow case stops reading early.
            else:
                self._send(404, b"not found")

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            if self.path == "/http/echo":
                self._send(200, self.headers.get("x-h2-test", "").encode() +
                           b":" + body)
            else:
                self._send(404, b"")

        def log_message(self, *args):
            pass

    origin = serve(Handler)
    cross = serve(Handler)
    base = f"http://127.0.0.1:{origin.server_port}/"
    cross_origin = f"http://127.0.0.1:{cross.server_port}"

    def expect(step, timeout=30):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"timed out waiting for {step}")
            try:
                message = steps.get(timeout=remaining)
            except queue.Empty:
                continue
            print("STEP", json.dumps(message), flush=True)
            assert message["code"] == 0, message
            if message["step"] == step:
                return message

    # Chrome's profile singleton socket needs a short path on macOS.
    with tempfile.TemporaryDirectory(dir="/tmp" if custom else None) as directory:
        to_browser_read, to_browser_write = os.pipe()
        from_browser_read, from_browser_write = os.pipe()

        def pipes():
            # Chromium reads commands from fd 3 and writes replies to fd 4.
            reader = os.dup(to_browser_read)
            writer = os.dup(from_browser_write)
            os.dup2(reader, 3)
            os.dup2(writer, 4)

        log = open(Path(directory) / "browser.log", "w+")
        process = subprocess.Popen([
            str(browsers[0]), "--headless", "--no-sandbox",
            "--enable-logging=stderr", "--v=0",
            "--autoplay-policy=no-user-gesture-required",
            "--remote-debugging-pipe", f"--user-data-dir={directory}/profile",
            "about:blank",
        ], stdout=log, stderr=log, preexec_fn=pipes, pass_fds=(3, 4))
        os.close(to_browser_read)
        os.close(from_browser_write)
        cdp = Cdp(to_browser_write, from_browser_read, steps)
        passed = False
        try:
            def open_tab(scenario):
                target = cdp.send("Target.createTarget",
                                  {"url": "about:blank"})["targetId"]
                session = cdp.send("Target.attachToTarget",
                                   {"targetId": target,
                                    "flatten": True})["sessionId"]
                cdp.send("Network.enable", session=session)
                cdp.send("Runtime.enable", session=session)
                navigate(session, scenario)
                return target, session

            def navigate(session, scenario):
                cdp.send("Page.navigate",
                         {"url": f"{base}?cross={cross_origin}#{scenario}"},
                         session=session)

            def reload(session, scenario):
                # Select the next scenario in place, then really refresh.
                cdp.send("Runtime.evaluate", {
                    "expression":
                        f"history.replaceState(null, '', '#{scenario}')",
                }, session=session)
                cdp.send("Page.reload", {"ignoreCache": False},
                         session=session)

            def offline(session, value):
                cdp.send("Network.emulateNetworkConditions", {
                    "offline": value, "latency": 0,
                    "downloadThroughput": -1, "uploadThroughput": -1,
                }, session=session)

            # Default network: offline and recovery reach the App.
            target, session = open_tab("netif")
            expect("netif-online")
            offline(session, True)
            expect("netif-offline")
            offline(session, False)
            expect("netif-restored")
            expect("destroy")
            cdp.send("Target.closeTarget", {"targetId": target})

            # HTTP semantics against real servers, including CORS.
            target, session = open_tab("http")
            expect("http-overflow", timeout=60)
            expect("destroy")
            cdp.send("Target.closeTarget", {"targetId": target})

            # Real WebCodecs decode, repeat playback, display and speaker.
            target, session = open_tab("media")
            decoded = expect("media-decode", timeout=60)
            if not custom:
                assert decoded["detail"].startswith("unsupported"), decoded
            else:
                assert decoded["detail"].startswith("pass1 rc=0"), decoded
            expect("speaker", timeout=60)
            expect("destroy")
            cdp.send("Target.closeTarget", {"targetId": target})

            # Filesystem: commit barriers survive a reload without shutdown.
            target, session = open_tab("fs-write")
            expect("fs-write")
            reload(session, "fs-verify")
            expect("fs-verify")
            expect("destroy")

            # A second tab cannot open the root while the first holds it.
            reload(session, "fs-hold")
            expect("fs-hold")
            second, _ = open_tab("fs-busy")
            expect("fs-busy")
            expect("destroy")
            cdp.send("Target.closeTarget", {"targetId": second})

            # Clearing is committed and survives a reload.
            reload(session, "fs-clear")
            expect("fs-clear")
            expect("destroy")
            reload(session, "fs-empty")
            expect("fs-empty")
            expect("destroy")

            # Quota exhaustion is reported as NO_SPACE with the browser error.
            reload(session, "fs-quota")
            expect("fs-quota")
            expect("destroy")
            cdp.send("Target.closeTarget", {"targetId": target})
            passed = True
        finally:
            try:
                cdp.send("Browser.close")
            except Exception:  # pylint: disable=broad-except
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            if not passed:
                log.seek(0)
                print(log.read()[-12000:])
            log.close()
            origin.shutdown()
            cross.shutdown()
    print("WEB_BROWSER_PLATFORM PASS")


if __name__ == "__main__":
    main()
