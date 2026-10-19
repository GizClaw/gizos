"""Run the real Worker-backed Audio PAL with Chromium's deterministic mic."""

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


def run_audio(browser, profile, url):
    incoming, outgoing = os.pipe(), os.pipe()
    events = queue.Queue()

    def pipes():
        reader, writer = os.dup(incoming[0]), os.dup(outgoing[1])
        os.dup2(reader, 3)
        os.dup2(writer, 4)

    with (profile / "browser.log").open("w") as log:
        proc = subprocess.Popen(
            [str(browser), "--headless", "--no-sandbox", "--remote-debugging-pipe",
             "--autoplay-policy=no-user-gesture-required", "--use-fake-device-for-media-stream",
             "--use-fake-ui-for-media-stream", f"--user-data-dir={profile}/data", "about:blank"],
            preexec_fn=pipes, pass_fds=(3, 4), stdout=log, stderr=log)
        os.close(incoming[0])
        os.close(outgoing[1])
        cdp = Cdp(incoming[1], outgoing[0], events)
        cases, summary, teardown, exited = [], None, None, None
        try:
            target = cdp.send("Target.createTarget", {"url": "about:blank"})["targetId"]
            session = cdp.send("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
            cdp.send("Runtime.enable", session=session)
            cdp.send("Page.enable", session=session)
            cdp.send("Page.navigate", {"url": url}, session=session)
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                try:
                    message = events.get(timeout=1)
                except queue.Empty:
                    continue
                if message.get("method") == "Runtime.exceptionThrown":
                    raise AssertionError(message)
                if message.get("method") != "Runtime.consoleAPICalled":
                    continue
                line = " ".join(str(arg.get("value", arg.get("description", "")))
                                for arg in message["params"]["args"])
                print(line, flush=True)
                if "Aborted(" in line or "Uncaught " in line:
                    raise AssertionError(line)
                if line.startswith("H2_PAL_AUDIO_CASE "):
                    cases.append(json.loads(line[len("H2_PAL_AUDIO_CASE "):]))
                elif line.startswith("H2_PAL_AUDIO_SUMMARY "):
                    summary = json.loads(line[len("H2_PAL_AUDIO_SUMMARY "):])
                elif line.startswith("H2_PAL_AUDIO_TEARDOWN "):
                    teardown = line
                elif line.startswith("H2_PAL_AUDIO_EXIT "):
                    exited = line
                    break
            else:
                raise TimeoutError("Audio WASM exceeded 120 seconds")
            assert exited == "H2_PAL_AUDIO_EXIT 0", exited
            assert teardown == "H2_PAL_AUDIO_TEARDOWN rc=0", teardown
            assert summary is not None
            return cases, summary
        finally:
            if proc.poll() is None:
                try:
                    cdp.send("Browser.close")
                except RuntimeError:
                    pass
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            cdp._write.close()
            with cdp._condition:
                cdp._condition.wait_for(lambda: None in cdp._replies, timeout=2)
            cdp._read.close()


def main():
    archive, registry = Path(sys.argv[1]), Path(sys.argv[2])
    expected = re.findall(r'H2_PAL_AUDIO_CASE\(\w+, "([^"]+)"\)', registry.read_text())
    assert len(expected) == len(set(expected)) == 24
    with prepared_archive(archive.resolve()) as root, tempfile.TemporaryDirectory(
            prefix="pal-audio-browser-") as directory:
        server = ThreadingHTTPServer(
            ("127.0.0.1", 0), make_handler(root, read_header_policy(root), frozenset()))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            cases, summary = run_audio(
                find_browser(), Path(directory),
                f"http://127.0.0.1:{server.server_address[1]}/")
            assert [case["id"] for case in cases] == expected
            assert all(case["status"] == "PASS" and case["detail"] == 0 for case in cases)
            assert summary["platform"] == "wasm-chromium" and summary["worker"] == 1
            assert summary["passed"] == 24 and summary["failed"] == 0 and summary["blocked"] == 0
            assert summary["mic_frames"] >= 2 and summary["speaker_frames"] >= 2
            assert summary["mic_peak"] > 0 and summary["output_peak"] > 0
            assert summary["stability_elapsed_ms"] >= 30000
            if os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
                path = Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"]) / "qualified.json"
                path.write_text(json.dumps({"summary": summary, "cases": cases}, indent=2) + "\n")
            print("PAL_AUDIO_WASM passed=24 failed=0 blocked=0 qualified=1")
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
