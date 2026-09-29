"""Qualify the real Worker provider with an explicitly selected AAC browser."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path("tools/bazel").resolve()))
from web_archive_browser_test import Cdp, find_browser
from web_archive_server import prepared_archive, make_handler, read_header_policy


def run_decoder(browser, profile, url):
    incoming, outgoing = os.pipe(), os.pipe()
    events = queue.Queue()
    def pipes():
        reader, writer = os.dup(incoming[0]), os.dup(outgoing[1])
        os.dup2(reader, 3); os.dup2(writer, 4)
    with (profile / "browser.log").open("w") as log:
        proc = subprocess.Popen([str(browser), "--headless", "--no-sandbox", "--remote-debugging-pipe",
                                 "--autoplay-policy=no-user-gesture-required", f"--user-data-dir={profile}/data", "about:blank"],
                                preexec_fn=pipes, pass_fds=(3, 4), stdout=log, stderr=log)
        os.close(incoming[0]); os.close(outgoing[1])
        cdp = Cdp(incoming[1], outgoing[0], events)
        cases, result = [], None
        try:
            identity = cdp.send("Browser.getVersion")
            target = cdp.send("Target.createTarget", {"url": "about:blank"})["targetId"]
            session = cdp.send("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
            cdp.send("Runtime.enable", session=session)
            cdp.send("Page.enable", session=session)
            cdp.send("Page.navigate", {"url": url}, session=session)
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                try: message = events.get(timeout=1)
                except queue.Empty: continue
                if message.get("method") == "Runtime.exceptionThrown":
                    raise AssertionError(message)
                if message.get("method") != "Runtime.consoleAPICalled": continue
                line = " ".join(str(a.get("value", a.get("description", ""))) for a in message["params"]["args"])
                print(line, flush=True)
                if "Aborted(" in line or "Uncaught " in line: raise AssertionError(line)
                if line.startswith("H2_ADEC_CASE "): cases.append(json.loads(line[13:]))
                if line.startswith("H2_ADEC_REPORT "): result = json.loads(line[15:])
                if line.startswith("H2_ADEC_EXIT "):
                    exit_code = int(line.removeprefix("H2_ADEC_EXIT "))
                    break
            else: raise TimeoutError("Audio Decoder exceeded 90s")
            assert result is not None
            result["browser_pid"] = proc.pid
            result["exit_code"] = exit_code
            result["browser"] = identity
            return cases, result
        finally:
            if proc.poll() is None:
                try: cdp.send("Browser.close")
                except RuntimeError: pass
            try: proc.wait(timeout=10)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=5)
            cdp._write.close()
            # EOF wakes the reader before the next Chromium instance opens pipes.
            with cdp._condition:
                cdp._condition.wait_for(lambda: None in cdp._replies, timeout=2)
            cdp._read.close()


def main():
    archive, registry = Path(sys.argv[1]), Path(sys.argv[2])
    expected = re.findall(r'H2_PAL_ADEC_CASE\("([^"]+)"', registry.read_text())
    assert expected and len(expected) == len(set(expected))
    if not os.environ.get("H2_WEB_TEST_BROWSER"):
        raise ValueError("H2_WEB_TEST_BROWSER must select an AAC-capable browser; pinned open-source Chromium has no AAC codec")
    browser = find_browser()
    with prepared_archive(archive.resolve()) as root, tempfile.TemporaryDirectory(prefix="pal-audio-decoder-browser-") as directory:
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(root, read_header_policy(root), frozenset()))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            cases, result = run_decoder(browser, Path(directory), f"http://127.0.0.1:{server.server_address[1]}/")
            result.update(cases=cases, archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                          browser_executable_sha256=hashlib.sha256(browser.read_bytes()).hexdigest(),
                          observation_finished_at_utc=datetime.now(timezone.utc).isoformat())
            if os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
                output = Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"])
                (output / "qualified.json").write_text(json.dumps(result, indent=2) + "\n")
                (output / "browser.log").write_bytes((Path(directory) / "browser.log").read_bytes())
            assert [c["id"] for c in cases] == expected
            assert all(c["status"] == "PASS" and c["detail"] == 0 for c in cases)
            for k, v in dict(contract=1, operations=8, passed=len(expected), failed=0, blocked=0, retained=0, qualified=1, rc=0, teardown=0, worker=1, exit_code=0).items():
                assert result[k] == v, (k, result)
            print(f"PAL_ADEC_WASM operations=8 passed={len(expected)} failed=0 blocked=0 qualified=1")
        finally:
            server.shutdown(); server.server_close()
            log = Path(directory) / "browser.log"
            if log.exists() and os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
                (Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"]) / "browser.log").write_bytes(log.read_bytes())

if __name__ == "__main__": main()
