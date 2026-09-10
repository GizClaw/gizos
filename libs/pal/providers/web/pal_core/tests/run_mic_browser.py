"""Exercise production getUserMedia/AudioWorklet with a synthetic Chrome device."""

import http.server
import json
import math
import os
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import tempfile
import threading
import wave


def main():
    files = {p.name: p for arg in sys.argv[1:] for item in arg.split()
             for p in [Path(item).resolve()]}
    assert set(files) == {"mic_browser_test.js", "mic_browser_test.wasm"}
    browsers = list(Path(os.environ["TEST_SRCDIR"]).glob(
        "*h2_playwright_chromium*/chrome-*/headless_shell"))
    assert len(browsers) == 1
    token = secrets.token_hex(16)
    result = []
    done = threading.Event()
    page = """<!doctype html><script>
const lines = [];
let reported = false;
function report(code) {
  if (reported) return;
  reported = true;
  fetch('/RESULT', {method:'POST', body:JSON.stringify({code, lines})});
}
var Module = {print: text => {lines.push(text); console.log(text);},
              printErr: text => {lines.push(text); console.error(text);},
              onExit: report, onAbort: text => {lines.push(text); report(1);}};
</script><script src='/mic_browser_test.js'></script>""".replace("RESULT", token)

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/":
                payload, mime = page.encode(), "text/html"
            elif self.path[1:] in files:
                payload = files[self.path[1:]].read_bytes()
                mime = "application/wasm" if self.path.endswith(".wasm") else "text/javascript"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            if self.path != "/" + token or not 0 < length <= 65536:
                self.send_error(400)
                return
            result.append(json.loads(self.rfile.read(length)))
            self.send_response(204)
            self.end_headers()
            done.set()

        def log_message(self, *args):
            pass

    with tempfile.TemporaryDirectory() as directory:
        # Deliberately 48 kHz: verify native resampling into the 16 kHz context.
        wav = Path(directory) / "microphone.wav"
        with wave.open(str(wav), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(48000)
            output.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(
                2 * math.pi * 440 * i / 48000))) for i in range(48000 * 2)))
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with (Path(directory) / "browser.log").open("w+") as log:
                process = subprocess.Popen([
                    str(browsers[0]), "--headless", "--no-sandbox",
                    "--autoplay-policy=no-user-gesture-required",
                    "--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
                    f"--use-file-for-fake-audio-capture={wav}",
                    f"--user-data-dir={directory}/profile",
                    f"http://127.0.0.1:{server.server_port}/",
                ], stdout=log, stderr=log)
                try:
                    assert done.wait(45), "microphone browser test timed out"
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
            print(json.dumps(result))
            assert result and result[0]["code"] == 0, result
            assert any("WEB_MIC PCM=" in line for line in result[0]["lines"])
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == "__main__":
    main()
