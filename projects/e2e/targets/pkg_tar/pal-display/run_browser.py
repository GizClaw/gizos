"""Run the real Worker-backed Display provider in isolated Chromium."""
import json
import hashlib
import base64
import struct
import zlib
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


def run_display(browser, profile, url):
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
        cases, captures, result = [], [], None
        try:
            target = cdp.send("Target.createTarget", {"url": "about:blank"})["targetId"]
            session = cdp.send("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
            cdp.send("Runtime.enable", session=session)
            cdp.send("Page.enable", session=session)
            cdp.send("Emulation.setDeviceMetricsOverride", {"width":96,"height":200,"deviceScaleFactor":1,"mobile":False}, session=session)
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
                if line.startswith("H2_DISPLAY_OBSERVE "):
                    pending = cdp.send("Runtime.evaluate", {"expression":"JSON.stringify({expected:h2DisplayPending.expected,width:h2DisplayPending.width,height:h2DisplayPending.height,brightness:h2DisplayPending.brightness,id:h2DisplayPending.id})","returnByValue":True},session=session)
                    expected = json.loads(pending["result"]["value"])
                    capture = cdp.send("Page.captureScreenshot", {"format":"png","clip":{"x":0,"y":0,"width":96,"height":80,"scale":1}},session=session)
                    png = base64.b64decode(capture["data"])
                    verify_pixels(png,expected)
                    name = f"{len(captures):02}-{expected['id']}-{expected['brightness']}.png"
                    (profile / name).write_bytes(png)
                    captures.append({"case_id":expected['id'],"brightness":expected['brightness'],"sha256":hashlib.sha256(png).hexdigest()})
                    if os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
                        (Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"]) / name).write_bytes(png)
                    cdp.send("Runtime.evaluate", {"expression":"h2DisplayPending.complete(0)"},session=session)
                if line.startswith("H2_DISPLAY_CASE "): cases.append(json.loads(line[16:]))
                if line.startswith("H2_DISPLAY_REPORT "): result = json.loads(line[18:])
                if line.startswith("H2_DISPLAY_EXIT "):
                    assert line == "H2_DISPLAY_EXIT 0", line
                    break
            else: raise TimeoutError("Display exceeded 90s")
            assert result is not None
            result["browser_pid"] = proc.pid
            result["composited_screenshots"] = captures
            assert len(captures) == 23
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


def verify_pixels(png, expected):
    """Decode the actual Chrome composited PNG with stdlib, then compare all pixels."""
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    cursor, compressed = 8, bytearray()
    while cursor < len(png):
        length = struct.unpack('>I',png[cursor:cursor+4])[0]
        kind, payload = png[cursor+4:cursor+8], png[cursor+8:cursor+8+length]
        if kind == b'IHDR': width,height,depth,color,_,_,interlace=struct.unpack('>IIBBBBB',payload)
        if kind == b'IDAT': compressed.extend(payload)
        cursor += length+12
    assert (width,height,depth,interlace) == (96,80,8,0)
    assert color in (2,6), color
    channels = 3 if color == 2 else 4
    raw, previous, offset = zlib.decompress(compressed), bytearray(width*channels), 0
    for y in range(height):
        method, row = raw[offset], bytearray(raw[offset+1:offset+1+width*channels])
        offset += 1+width*channels
        for i in range(len(row)):
            a, b, c = (row[i-channels] if i>=channels else 0), previous[i], (previous[i-channels] if i>=channels else 0)
            if method == 1: prediction=a
            elif method == 2: prediction=b
            elif method == 3: prediction=(a+b)//2
            elif method == 4:
                p=a+b-c; distances=(abs(p-a),abs(p-b),abs(p-c))
                prediction=(a,b,c)[distances.index(min(distances))]
            else:
                assert method == 0
                prediction=0
            row[i]=(row[i]+prediction)&255
        for x in range(width):
            value=expected['expected'][y*width+x]
            target=(((value>>11)&31)*255//31,((value>>5)&63)*255//63,(value&31)*255//31)
            for c in range(3):
                want=target[c]*expected['brightness']//100
                assert abs(row[x*channels+c]-want)<=3, (expected['id'],x,y,c,row[x*channels+c],want)
        previous=row


def main():
    archive, registry = Path(sys.argv[1]), Path(sys.argv[2])
    expected = re.findall(r'H2_PAL_DISPLAY_CASE\("([^"]+)"', registry.read_text())
    assert expected and len(expected) == len(set(expected))
    with prepared_archive(archive.resolve()) as root, tempfile.TemporaryDirectory(prefix="pal-display-browser-") as directory:
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(root, read_header_policy(root), frozenset()))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            cases, result = run_display(find_browser(), Path(directory), f"http://127.0.0.1:{server.server_address[1]}/")
            assert [c["id"] for c in cases] == expected
            assert all(c["status"] == "PASS" and c["rc"] == 0 for c in cases)
            for k, v in dict(contract=1, operations=6, passed=len(expected), failed=0, not_run=0, observations=23, complete=1, qualified=1, rc=0, cleanup=0, teardown=0).items():
                assert result[k] == v, (k, result)
            result["cases"] = cases
            if os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
                (Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"]) / "qualified.json").write_text(json.dumps(result, indent=2) + "\n")
            print(f"PAL_DISPLAY_WASM operations=6 passed={len(expected)} failed=0 qualified=1")
        finally: server.shutdown(); server.server_close()

if __name__ == "__main__": main()
