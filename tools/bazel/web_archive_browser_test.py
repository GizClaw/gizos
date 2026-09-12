#!/usr/bin/env python3
"""Run one Bazel-built Web archive in real headless Chromium and judge it.

The archive is extracted and served exactly as `web_archive_serve` does. The
harness drives Chromium over the DevTools pipe: it opens the page, clicks
`#start` with a user gesture when the shell has one (microphone, audio and
Web Serial need it), and reads every console line, uncaught exception,
browser log entry and new line of page text. The test passes once every `--pass` pattern has appeared
and fails on any `--fail` pattern, uncaught exception, Emscripten abort or
timeout. `--offline` switches the page offline when a marker appears and
back online at the next one, so network recovery paths run for real.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import queue
import re
import select
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from web_archive_server import (  # noqa: E402  pylint: disable=wrong-import-position
    make_handler,
    prepared_archive,
    read_header_policy,
)

CANVAS_PIXELS = """(() => {
  const canvas = document.getElementById('canvas');
  if (!canvas || !canvas.width) return -1;
  const data = canvas.getContext('2d').getImageData(
      0, 0, canvas.width, canvas.height).data;
  let count = 0;
  for (let i = 0; i < data.length; i += 4)
    if (data[i] | data[i + 1] | data[i + 2]) ++count;
  return count;
})()"""

# Canvas pixel -> viewport CSS coordinates for Input.dispatchMouseEvent.
CANVAS_POINT = """(() => {
  const canvas = document.getElementById('canvas');
  const rect = canvas.getBoundingClientRect();
  return [rect.left + (%d + 0.5) * rect.width / canvas.width,
          rect.top + (%d + 0.5) * rect.height / canvas.height];
})()"""

ELEMENT_CENTER = """(() => {
  const element = document.querySelector(%s);
  if (!element) return null;
  const rect = element.getBoundingClientRect();
  return [rect.left + rect.width / 2, rect.top + rect.height / 2];
})()"""

DEFAULT_FAIL = [
    r"Aborted\(",
    r"RuntimeError: ",
    r"Uncaught ",
]


class Cdp:
    """Minimal Chrome DevTools Protocol client over --remote-debugging-pipe."""

    def __init__(self, write_fd: int, read_fd: int, events: queue.Queue):
        self._write = os.fdopen(write_fd, "wb", buffering=0)
        self._read = os.fdopen(read_fd, "rb", buffering=0)
        self._events = events
        self._next_id = 0
        self._replies: dict = {}
        self._condition = threading.Condition()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self) -> None:
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
                else:
                    self._events.put(message)

    def send(self, method: str, params: dict | None = None,
             session: str | None = None) -> dict:
        with self._condition:
            self._next_id += 1
            message_id = self._next_id
        message = {"id": message_id, "method": method, "params": params or {}}
        if session:
            message["sessionId"] = session
        self._write.write(json.dumps(message).encode() + b"\0")
        with self._condition:
            if not self._condition.wait_for(
                    lambda: message_id in self._replies or None in self._replies,
                    timeout=30):
                raise RuntimeError(f"{method} timed out")
            if message_id not in self._replies:
                raise RuntimeError("browser closed the DevTools pipe")
            reply = self._replies.pop(message_id)
        if "error" in reply:
            raise RuntimeError(f"{method} failed: {reply['error']}")
        return reply.get("result", {})


def event_lines(message: dict) -> list[str]:
    method = message.get("method")
    params = message.get("params", {})
    if method == "Runtime.consoleAPICalled":
        text = " ".join(str(arg.get("value", arg.get("description", "")))
                        for arg in params.get("args", []))
        return [f"[{params.get('type')}] {text}"]
    if method == "Runtime.exceptionThrown":
        details = params.get("exceptionDetails", {})
        description = details.get("exception", {}).get("description")
        return [f"[exception] Uncaught {description or details.get('text')}"]
    if method == "Log.entryAdded":
        entry = params.get("entry", {})
        return [f"[browser-{entry.get('level')}] {entry.get('text')}"]
    if method == "Inspector.targetCrashed":
        return ["[exception] Uncaught renderer crash"]
    return []


@contextlib.contextmanager
def pion_server(binary: str | None):
    """Start the repository WebRTC fixture and yield its endpoints."""
    if not binary:
        yield None
        return
    process = subprocess.Popen([
        str(Path(binary).resolve()), "-listen=127.0.0.1:0",
        "-stun-listen=127.0.0.1:0", "-turn-listen=127.0.0.1:0",
        "-candidate-ip=127.0.0.1",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if not select.select([process.stdout], [], [], 0.1)[0]:
                continue
            line = process.stdout.readline().decode()
            if not line:
                raise RuntimeError("WebRTC fixture exited before ready")
            match = re.search(r"H2_WEBRTC_TEST_SERVER_READY http=(\S+) stun=(\S+) ", line)
            if match:
                yield {"http": "http://" + match[1], "stun": "stun:" + match[2]}
                return
        raise TimeoutError("WebRTC fixture startup timeout")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def find_browser() -> Path:
    custom = os.environ.get("H2_WEB_TEST_BROWSER")
    if custom:
        return Path(custom)
    candidates = list(Path(os.environ["TEST_SRCDIR"]).glob(
        "*h2_playwright_chromium*/chrome-*/headless_shell"))
    if len(candidates) != 1:
        raise SystemExit(f"expected one pinned Chromium, found {candidates}")
    return candidates[0]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--pass", dest="passes", action="append", default=[],
                        help="regex every one of which must appear")
    parser.add_argument("--fail", dest="fails", action="append", default=[],
                        help="regex that fails the run when it appears")
    parser.add_argument("--query", default="",
                        help="query string appended to the page URL")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--no-start", action="store_true",
                        help="do not click #start")
    parser.add_argument("--offline", nargs=2, metavar=("DOWN", "UP"),
                        help="go offline at regex DOWN, online at regex UP")
    parser.add_argument("--pion", help="webrtc-test-server binary to start")
    parser.add_argument("--press", nargs=2, action="append", default=[],
                        metavar=("REGEX", "KEY"),
                        help="press and release KEY once REGEX appears")
    parser.add_argument("--tap", nargs=3, action="append", default=[],
                        metavar=("REGEX", "X", "Y"),
                        help="tap #canvas pixel X,Y once REGEX appears")
    parser.add_argument("--click", nargs=2, action="append", default=[],
                        metavar=("REGEX", "SELECTOR"),
                        help="mouse-click the page element SELECTOR once REGEX appears")
    parser.add_argument("--http-proxy-origin", action="append", default=[])
    parser.add_argument("--canvas-min", type=int, default=0,
                        help="require this many non-black #canvas pixels")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    browser = find_browser()
    events: queue.Queue = queue.Queue()
    lines: list[str] = []
    with pion_server(args.pion) as pion, \
            prepared_archive(args.archive.resolve()) as root, \
            tempfile.TemporaryDirectory(dir="/tmp" if os.environ.get(
                "H2_WEB_TEST_BROWSER") else None) as profile:
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(
            root, read_header_policy(root),
            frozenset(args.http_proxy_origin + ([pion["http"]] if pion else []))))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        query = args.query
        if pion:
            query = "&".join(filter(None, [query,
                                           "stun=" + pion["stun"],
                                           "signal=" + pion["http"]]))
        url = f"http://127.0.0.1:{server.server_address[1]}/"
        if query:
            url += "?" + query

        to_browser_read, to_browser_write = os.pipe()
        from_browser_read, from_browser_write = os.pipe()

        def pipes() -> None:
            reader = os.dup(to_browser_read)
            writer = os.dup(from_browser_write)
            os.dup2(reader, 3)
            os.dup2(writer, 4)

        log_path = Path(profile) / "browser.log"
        with log_path.open("w+") as log:
            process = subprocess.Popen([
                str(browser), "--headless", "--no-sandbox",
                "--remote-debugging-pipe",
                "--autoplay-policy=no-user-gesture-required",
                "--use-fake-ui-for-media-stream",
                "--use-fake-device-for-media-stream",
                f"--user-data-dir={profile}/profile", "about:blank",
            ], stdout=log, stderr=log, preexec_fn=pipes, pass_fds=(3, 4))
            os.close(to_browser_read)
            os.close(from_browser_write)
            cdp = Cdp(to_browser_write, from_browser_read, events)
            outcome = "timeout"
            try:
                target = cdp.send("Target.createTarget",
                                  {"url": "about:blank"})["targetId"]
                session = cdp.send("Target.attachToTarget", {
                    "targetId": target, "flatten": True})["sessionId"]
                for domain in ("Runtime", "Page", "Log", "Network", "Inspector"):
                    cdp.send(f"{domain}.enable", session=session)
                cdp.send("Page.navigate", {"url": url}, session=session)
                pending = [re.compile(pattern) for pattern in args.passes]
                fails = [re.compile(pattern)
                         for pattern in DEFAULT_FAIL + args.fails]
                offline_steps = [re.compile(p) for p in args.offline or []]
                presses = [(re.compile(p), key) for p, key in args.press]
                taps = [(re.compile(p), int(x), int(y)) for p, x, y in args.tap]
                clicks = [(re.compile(p), selector) for p, selector in args.click]
                offline_state = False
                started = args.no_start
                page_lines: set[str] = set()
                next_page_poll = 0.0
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    if not started:
                        state = cdp.send("Runtime.evaluate", {
                            "expression": "document.readyState + ':' + "
                                          "!!document.getElementById('start')",
                            "returnByValue": True}, session=session)
                        value = state.get("result", {}).get("value", "")
                        if value.startswith("complete"):
                            if value.endswith("true"):
                                cdp.send("Runtime.evaluate", {
                                    "expression":
                                        "document.getElementById('start').click()",
                                    "userGesture": True}, session=session)
                            started = True
                    new_lines: list[str] = []
                    try:
                        new_lines = event_lines(events.get(timeout=0.2))
                    except queue.Empty:
                        pass
                    # Shells often print to the page instead of the console.
                    if started and time.monotonic() >= next_page_poll:
                        next_page_poll = time.monotonic() + 0.5
                        page = cdp.send("Runtime.evaluate", {
                            "expression": "document.body ? document.body.innerText : ''",
                            "returnByValue": True}, session=session)
                        for text in str(page.get("result", {}).get(
                                "value", "")).splitlines():
                            text = text.strip()
                            if text and text not in page_lines:
                                page_lines.add(text)
                                new_lines.append(f"[page] {text}")
                    for line in new_lines:
                        lines.append(line)
                        print(line, flush=True)
                        if any(pattern.search(line) for pattern in fails):
                            outcome = "fail"
                            break
                        pending = [p for p in pending if not p.search(line)]
                        for press in list(presses):
                            if press[0].search(line):
                                presses.remove(press)
                                for kind in ("keyDown", "keyUp"):
                                    cdp.send("Input.dispatchKeyEvent", {
                                        "type": kind, "key": press[1],
                                        "code": press[1]}, session=session)
                                print(f"[harness] pressed {press[1]}", flush=True)
                        for click in list(clicks):
                            if click[0].search(line):
                                clicks.remove(click)
                                center = cdp.send("Runtime.evaluate", {
                                    "expression": ELEMENT_CENTER % json.dumps(click[1]),
                                    "returnByValue": True}, session=session)
                                point = center.get("result", {}).get("value")
                                if not point:
                                    print(f"[harness] no element {click[1]}", flush=True)
                                    outcome = "fail"
                                    break
                                for kind in ("mousePressed", "mouseReleased"):
                                    cdp.send("Input.dispatchMouseEvent", {
                                        "type": kind, "x": point[0], "y": point[1],
                                        "button": "left", "buttons": 1,
                                        "clickCount": 1}, session=session)
                                    time.sleep(0.08)
                                print(f"[harness] clicked {click[1]}", flush=True)
                        for tap in list(taps):
                            if tap[0].search(line):
                                taps.remove(tap)
                                rect = cdp.send("Runtime.evaluate", {
                                    "expression": CANVAS_POINT % (tap[1], tap[2]),
                                    "returnByValue": True}, session=session)
                                x, y = rect["result"]["value"]
                                for kind in ("mousePressed", "mouseReleased"):
                                    cdp.send("Input.dispatchMouseEvent", {
                                        "type": kind, "x": x, "y": y,
                                        "button": "left", "buttons": 1,
                                        "clickCount": 1}, session=session)
                                print(f"[harness] tapped {tap[1]},{tap[2]}", flush=True)
                        if offline_steps and offline_steps[0].search(line):
                            offline_state = not offline_state
                            offline_steps.pop(0)
                            cdp.send("Network.emulateNetworkConditions", {
                                "offline": offline_state, "latency": 0,
                                "downloadThroughput": -1,
                                "uploadThroughput": -1}, session=session)
                            print(f"[harness] offline={offline_state}", flush=True)
                    if outcome == "fail":
                        break
                    if not pending and not offline_steps:
                        outcome = "pass"
                        if args.canvas_min:
                            painted = cdp.send("Runtime.evaluate", {
                                "expression": CANVAS_PIXELS,
                                "returnByValue": True}, session=session)
                            count = painted.get("result", {}).get("value", -1)
                            print(f"[harness] canvas non-black pixels={count}")
                            if not isinstance(count, int) or count < args.canvas_min:
                                outcome = "fail"
                        break
                if outcome == "timeout":
                    print(f"[harness] missing: {[p.pattern for p in pending]}")
                    with contextlib.suppress(Exception):
                        page = cdp.send("Runtime.evaluate", {
                            "expression": "document.body.innerText.slice(0, 400)",
                            "returnByValue": True}, session=session)
                        print("[harness] page text: " +
                              repr(page.get("result", {}).get("value")))
            finally:
                with contextlib.suppress(Exception):
                    cdp.send("Browser.close")
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                server.shutdown()
                if outcome != "pass":
                    log.seek(0)
                    print(log.read()[-8000:])
    print(f"[harness] outcome={outcome}")
    return 0 if outcome == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
