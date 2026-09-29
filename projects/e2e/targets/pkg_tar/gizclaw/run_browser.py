"""Run GizClaw's real portable App in a Chromium pthread Worker."""
import argparse
import hashlib
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit

root = Path(__file__).absolute().parents[5]
sys.path.insert(0, str(root / "tools/bazel"))
from web_archive_browser_test import Cdp, find_browser
from web_archive_server import prepared_archive, make_handler, read_header_policy
sys.path.insert(0, str(root / "projects/e2e/apps/gizclaw"))
import api_coverage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--pcm", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"))
    parser.add_argument("--profile", required=True)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    api_coverage.validate_inventory(api_coverage.requirements(),
        (api_coverage.repository_root() / "libs/gizclaw/tests/public_api.inc").read_text())
    if args.output is None:
        parser.error("--output is required outside Bazel tests")
    fixture = {key: os.environ.get(env, "") for key, env in (
        ("endpoint", "H2_GIZCLAW_E2E_ENDPOINT"), ("token", "H2_GIZCLAW_E2E_REGISTRATION_TOKEN"),
        ("api_url", "H2_GIZCLAW_E2E_DEVICE_API_URL"), ("audio_url", "H2_GIZCLAW_E2E_AUDIO_URL"))}
    if not all(fixture.values()):
        parser.error("explicit E2E endpoint, token and device API/audio fixtures required")
    args.output.mkdir(parents=True, exist_ok=True)
    archive_copy = args.output / "app.web.tar"
    shutil.copyfile(args.archive, archive_copy)
    args.archive = archive_copy
    with prepared_archive(args.archive.resolve()) as archive, tempfile.TemporaryDirectory(prefix="gizclaw-browser-") as tmp:
        class Handler(make_handler(archive, read_header_policy(archive))):
            def do_GET(self):
                if self.path not in ("/fixture.json", "/voice.pcm"):
                    return super().do_GET()
                body = json.dumps(fixture).encode() if self.path == "/fixture.json" else args.pcm.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "application/json" if self.path == "/fixture.json" else "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *unused):
                pass
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        to_read, to_write = os.pipe()
        from_read, from_write = os.pipe()
        def pipes():
            reader, writer = os.dup(to_read), os.dup(from_write)
            os.dup2(reader, 3)
            os.dup2(writer, 4)
        process = subprocess.Popen([str(find_browser()), "--headless", "--no-sandbox",
            "--remote-debugging-pipe", "--autoplay-policy=no-user-gesture-required",
            "--user-data-dir=" + tmp, "about:blank"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=pipes, pass_fds=(3, 4))
        os.close(to_read); os.close(from_write)
        events = queue.Queue()
        cdp = Cdp(to_write, from_read, events)
        lines, exit_code, final = [], None, None
        requests, network_failures = {}, []
        try:
            target = cdp.send("Target.createTarget", {"url": "about:blank"})["targetId"]
            session = cdp.send("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
            cdp.send("Runtime.enable", session=session)
            cdp.send("Page.enable", session=session)
            cdp.send("Network.enable", session=session)
            cdp.send("Page.navigate", {"url": f"http://127.0.0.1:{server.server_port}/"}, session=session)
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                try: event = events.get(timeout=0.5)
                except queue.Empty:
                    if process.poll() is not None: raise RuntimeError("browser exited early")
                    continue
                if event.get("method") == "Network.requestWillBeSent":
                    params = event["params"]
                    requests[params["requestId"]] = urlsplit(params["request"]["url"]).hostname
                if event.get("method") == "Network.loadingFailed":
                    params = event["params"]
                    network_failures.append({"host": requests.get(params["requestId"]),
                        **{key: params[key] for key in ("errorText", "blockedReason", "corsErrorStatus", "type") if key in params}})
                if event.get("method") == "Runtime.exceptionThrown":
                    raise RuntimeError("browser exception; inspect captured test log")
                if event.get("method") != "Runtime.consoleAPICalled": continue
                text = " ".join(str(arg.get("value", arg.get("description", ""))) for arg in event["params"].get("args", []))
                text = text.replace(fixture["token"], "[REDACTED]")
                text = re.sub(r"https?://[^\s)]+", "[redacted-url]", text)
                lines.append(text)
                with (args.output / "test.log").open("a") as log:
                    log.write(text + "\n")
                if text.startswith("H2_GIZCLAW_PLATFORM_FINAL "): final = text
                if text.startswith("H2_GIZCLAW_PROCESS_EXIT "):
                    exit_code = int(text.split()[-1]); break
                if text.startswith("H2_GIZCLAW_FIXTURE_LOAD_FAILED") or "Aborted(" in text:
                    raise RuntimeError("WASM fixture/runtime failure")
            state = cdp.send("Runtime.evaluate", {"expression": "({isolated:crossOriginIsolated})", "returnByValue": True}, session=session)["result"]["value"]
            audit = api_coverage.audit([line + "\n" for line in lines], api_coverage.requirements(),
                endpoint=fixture["endpoint"], backend="h2peer", profile=args.profile,
                platform="wasm-chromium", process_exit_code=exit_code if exit_code is not None else 1)
            (args.output / "coverage.json").write_text(json.dumps(audit, indent=2) + "\n")
            clean = exit_code == 0 and final == "H2_GIZCLAW_PLATFORM_FINAL rc=0 teardown=0 retained=0 worker=1" and state.get("isolated")
            evidence = dict(qualified=bool(clean and audit["valid"]), network_failures=network_failures, browser=cdp.send("Browser.getVersion"), state=state,
                process_exit_code=exit_code, platform_final=final,
                archive_sha256=hashlib.sha256(args.archive.read_bytes()).hexdigest(),
                pcm_sha256=hashlib.sha256(args.pcm.read_bytes()).hexdigest())
            (args.output / "environment.json").write_text(json.dumps(evidence, indent=2) + "\n")
            print(json.dumps({"process_exit_code": exit_code, "platform_final": final, "state": state}))
            return 0 if clean and audit["valid"] else 1
        finally:
            (args.output / "test.log").write_text("\n".join(lines) + "\n")
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
