"""Restart real Chromium while preserving its profile and the HTTP origin."""
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


def run_phase(browser, profile, url, phase, nonce):
    incoming, outgoing = os.pipe(), os.pipe()
    events = queue.Queue()
    def pipes():
        reader, writer = os.dup(incoming[0]), os.dup(outgoing[1])
        os.dup2(reader, 3); os.dup2(writer, 4)
    with (profile / f"browser-{phase}.log").open("w") as log:
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
            cdp.send("Page.navigate", {"url": url + f"?phase={phase}&nonce={nonce}"}, session=session)
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
                if line.startswith("H2_STORAGE_CASE "): cases.append(json.loads(line[16:]))
                if line.startswith("H2_STORAGE_PHASE "): result = json.loads(line[17:])
                if line.startswith("H2_STORAGE_EXIT "):
                    assert line == "H2_STORAGE_EXIT 0", line
                    break
            else: raise TimeoutError(f"storage phase {phase} exceeded 90s")
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
    expected = {name: int(phase) for name, phase in re.findall(r'H2_PAL_STORAGE_CASE\("([^"]+)", ([12])\)', registry.read_text())}
    nonce, all_cases, phases = secrets.randbits(32), [], []
    with prepared_archive(archive.resolve()) as root, tempfile.TemporaryDirectory(prefix="pal-storage-browser-") as directory:
        profile = Path(directory)
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(root, read_header_policy(root), frozenset()))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            for phase in (1, 2):
                cases, result = run_phase(find_browser(), profile, f"http://127.0.0.1:{server.server_address[1]}/", phase, nonce)
                ids = {name for name, value in expected.items() if value == phase}
                assert len(cases) == len(ids) and {c["id"] for c in cases} == ids
                assert all(c["phase"] == phase and c["nonce"] == nonce and c["status"] == "PASS" and c["rc"] == 0 for c in cases)
                assert result["phase"] == phase and result["nonce"] == nonce and result["passed"] == len(ids)
                assert result["failed"] == result["blocked"] == result["cleanup"] == result["fs_close"] == result["platform_destroy"] == 0
                all_cases.extend(cases); phases.append(result)
            assert phases[0]["browser_pid"] != phases[1]["browser_pid"]
            assert len(all_cases) == len(expected) == 30
            output = {"platform": "wasm-chromium", "contract": 1, "operations": 28, "qualified": True, "passed": 30, "failed": 0, "blocked": 0, "cases": all_cases, "phases": phases}
            if os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
                (Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"]) / "qualified.json").write_text(json.dumps(output, indent=2) + "\n")
            print("PAL_STORAGE_WASM operations=28 passed=30 failed=0 blocked=0 browser_restart=PASS qualified=1")
        finally: server.shutdown(); server.server_close()

if __name__ == "__main__": main()
