"""Run the real Worker-backed Crypto provider in isolated Chromium."""
import json
import os
from pathlib import Path
import queue
import re
import secrets
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path("tools/bazel").resolve()))
from web_archive_browser_test import Cdp, find_browser
from web_archive_server import prepared_archive, make_handler, read_header_policy


def run_crypto(browser, profile, url):
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
                if line.startswith("H2_CRYPTO_CASE "): cases.append(json.loads(line[15:]))
                if line.startswith("H2_CRYPTO_REPORT "): result = json.loads(line[17:])
                if line.startswith("H2_CRYPTO_EXIT "):
                    assert line == "H2_CRYPTO_EXIT 0", line
                    break
            else: raise TimeoutError("crypto exceeded 90s")
            assert result is not None
            result["browser_pid"] = proc.pid
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
    expected = re.findall(r'H2_PAL_CRYPTO_CASE\("([^"]+)"', registry.read_text())
    assert len(expected) == len(set(expected)) == 22
    with prepared_archive(archive.resolve()) as root, tempfile.TemporaryDirectory(prefix="pal-crypto-browser-") as directory:
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(root, read_header_policy(root), frozenset()))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            cases, result = run_crypto(find_browser(), Path(directory), f"http://127.0.0.1:{server.server_address[1]}/")
            assert [c["id"] for c in cases] == expected
            assert all(c["status"] == "PASS" and c["rc"] == 0 for c in cases)
            for k, v in dict(contract=1, operations=15, passed=22, failed=0, blocked=0, not_run=0, complete=1, qualified=1, rc=0, teardown=0).items():
                assert result[k] == v, (k, result)
            result["cases"] = cases
            if os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
                (Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"]) / "qualified.json").write_text(json.dumps(result, indent=2) + "\n")
            print("PAL_CRYPTO_WASM operations=15 passed=22 failed=0 blocked=0 qualified=1")
        finally: server.shutdown(); server.server_close()

if __name__ == "__main__": main()
