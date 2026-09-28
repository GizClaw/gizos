"""PAL WebRTC qualification against real Pion in pinned Chromium."""
import argparse
from http.server import ThreadingHTTPServer
import json
import hashlib
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlencode
import http.client
import re

root = Path(__file__).absolute().parents[5]
sys.path.insert(0, str(root / 'tools/bazel'))
sys.path.insert(0, str(root / 'libs/pal/providers/web/pal_core/tests'))
from web_archive_browser_test import Cdp, find_browser
from web_archive_server import prepared_archive, make_handler, read_header_policy
from run_webrtc_browser import ice_server

def validate(output, registry, authentication_session):
    expected = re.findall(r'H2_PAL_WEBRTC_CASE\(\w+, "([^"]+)"\)', Path(registry).read_text())
    cases = [json.loads(line.split(" ", 1)[1]) for line in output.splitlines() if line.startswith("H2_PAL_WEBRTC_CASE ")]
    summary = [json.loads(line.split(" ", 1)[1]) for line in output.splitlines() if line.startswith("H2_PAL_WEBRTC_SUMMARY ")]
    if [case["id"] for case in cases] != expected or not all(case["status"] == "PASS" for case in cases):
        raise RuntimeError("incomplete WebRTC case ledger")
    if len(summary) != 1 or summary[0] != dict(passed=len(expected), failed=0, blocked=0, retained_allocations=0, cleanup=0):
        raise RuntimeError("WebRTC qualification failed: " + repr(summary))
    authentication = next(case for case in cases if case["id"] == "fingerprint-rejected")
    witnesses = [json.loads(line.split(" ", 1)[1]) for line in output.splitlines()
                 if line.startswith("H2_PAL_WEBRTC_AUTH_WITNESS ")]
    if authentication.get("observed_error") != -17 or authentication.get("authentication_evidence") not in (1, 2):
        raise RuntimeError("missing precise certificate authentication verdict")
    if authentication["authentication_evidence"] == 2 and not any(
            witness.get("source") == "pion-dtls-typed-alert" and
            witness.get("direction") == "received" and
            witness.get("session_id") == authentication_session and
            witness.get("level") == 2 and witness.get("alert") in (42, 46) and
            witness.get("channels_opened") == 0 and witness.get("certificate_rejection") is True
            for witness in witnesses):
        raise RuntimeError("fixture authentication verdict lacks its session-bound typed rejection")
    return dict(cases=cases, summary=summary[0], authentication_witnesses=witnesses)


def run(args):
    with tempfile.TemporaryDirectory(prefix='h2-webrtc-browser-') as temp, \
            prepared_archive(Path(args.archive).resolve()) as archive, ice_server(args.server) as fixture:
        sessions = []
        class Handler(make_handler(archive, read_header_policy(archive))):
            def do_GET(self):
                if self.path != '/pion/authentication-witness':
                    return super().do_GET()
                if not sessions:
                    self.send_error(409)
                    return
                connection = http.client.HTTPConnection('127.0.0.1', fixture['http_port'], timeout=5)
                try:
                    connection.request('GET', '/session/' + sessions[-1] + '/authentication-witness')
                    response = connection.getresponse()
                    payload = response.read(8192)
                    if response.status == 200 and json.loads(payload).get('session_id') != sessions[-1]:
                        self.send_error(502)
                        return
                    self.send_response(response.status)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                finally:
                    connection.close()
            def do_POST(self):
                if self.path not in ('/pion/offer', '/pion/close'):
                    self.send_error(404)
                    return
                size = int(self.headers.get('Content-Length', '0'))
                if size > 32768:
                    self.send_error(413)
                    return
                connection = http.client.HTTPConnection('127.0.0.1', fixture['http_port'], timeout=20)
                try:
                    headers = {'Content-Type': 'application/sdp'}
                    for name in ('X-H2-Negotiated-ID', 'X-H2-Negotiated-Label',
                                 'X-H2-Negotiated-Ordered', 'X-H2-Negotiated-Reliable'):
                        if self.headers.get(name) is not None:
                            headers[name] = self.headers[name]
                    connection.request('POST', '/offer' if self.path == '/pion/offer' else '/session/' + sessions[-1] + '/close', self.rfile.read(size), headers)
                    response = connection.getresponse()
                    if response.getheader('X-H2-Session-ID'):
                        sessions.append(response.getheader('X-H2-Session-ID'))
                    payload = response.read(32769)
                    self.send_response(response.status)
                    self.send_header('Content-Type', 'application/sdp')
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                finally:
                    connection.close()
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        serving = threading.Thread(target=server.serve_forever, daemon=True)
        serving.start()
        url = f'http://127.0.0.1:{server.server_port}/?' + urlencode(dict(stun=fixture['stun']))
        to_read, to_write = os.pipe()
        from_read, from_write = os.pipe()

        def pipes():
            reader, writer = os.dup(to_read), os.dup(from_write)
            os.dup2(reader, 3)
            os.dup2(writer, 4)

        events = queue.Queue()
        process = subprocess.Popen([str(find_browser()), '--headless', '--no-sandbox',
                                    '--remote-debugging-pipe', '--autoplay-policy=no-user-gesture-required',
                                    '--user-data-dir=' + temp + '/profile', 'about:blank'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   preexec_fn=pipes, pass_fds=(3, 4))
        os.close(to_read)
        os.close(from_write)
        cdp = Cdp(to_write, from_read, events)
        lines = []
        try:
            target = cdp.send('Target.createTarget', {'url': 'about:blank'})['targetId']
            session = cdp.send('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']
            cdp.send('Runtime.enable', session=session)
            cdp.send('Page.enable', session=session)
            cdp.send('Page.navigate', {'url': url}, session=session)
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                try:
                    event = events.get(timeout=0.2)
                except queue.Empty:
                    if process.poll() is not None:
                        raise RuntimeError('Chromium exited early')
                    continue
                if event.get('method') == 'Runtime.exceptionThrown':
                    raise RuntimeError(event['params'])
                if event.get('method') != 'Runtime.consoleAPICalled':
                    continue
                text = ' '.join(str(arg.get('value', arg.get('description', ''))) for arg in event['params'].get('args', []))
                print(text, flush=True)
                lines.append(text)
                if 'Aborted(' in text:
                    raise RuntimeError('WASM aborted')
                if text.startswith('H2_PAL_WEBRTC_SUMMARY '):
                    browser_version = cdp.send('Browser.getVersion')
                    if args.evidence:
                        observed = dict(qualified=False, browser=browser_version,
                            cases=[json.loads(line.split(' ', 1)[1]) for line in lines if line.startswith('H2_PAL_WEBRTC_CASE ')],
                            summary=json.loads(text.split(' ', 1)[1]),
                            authentication_witnesses=[json.loads(line.split(' ', 1)[1]) for line in lines if line.startswith('H2_PAL_WEBRTC_AUTH_WITNESS ')],
                            fixture_sessions=sessions.copy(),
                            artifact_sha256=hashlib.sha256(Path(args.archive).read_bytes()).hexdigest(),
                            registry_sha256=hashlib.sha256(Path(args.cases).read_bytes()).hexdigest())
                        Path(args.evidence).write_text(json.dumps(observed, indent=2) + '\n')
                    evidence = validate('\n'.join(lines), args.cases, sessions[1] if len(sessions) > 1 else None)
                    evidence['fixture_sessions'] = sessions.copy()
                    evidence['platform'] = 'wasm-chromium'
                    evidence['browser'] = browser_version
                    state = cdp.send('Runtime.evaluate', {'expression': '({isolated:crossOriginIsolated,pending:Module.h2WebRtcPeers?.size||0})', 'returnByValue': True}, session=session)['result']['value']
                    if state != {'isolated': True, 'pending': 0}:
                        raise RuntimeError('browser request resources retained: ' + repr(state))
                    evidence['browser_state'] = state
                    evidence['artifact_sha256'] = hashlib.sha256(Path(args.archive).read_bytes()).hexdigest()
                    evidence['registry_sha256'] = hashlib.sha256(Path(args.cases).read_bytes()).hexdigest()
                    if args.evidence:
                        Path(args.evidence).write_text(json.dumps(evidence, indent=2) + '\n')
                    return
            raise RuntimeError('browser qualification timed out')
        finally:
            process.terminate()
            process.wait(timeout=10)
            server.shutdown()
            server.server_close()
            serving.join(timeout=2)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', required=True)
    parser.add_argument('--cases', required=True)
    parser.add_argument('--server', required=True)
    parser.add_argument('--evidence', default=str(Path(os.environ['TEST_UNDECLARED_OUTPUTS_DIR']) / 'qualified.json') if 'TEST_UNDECLARED_OUTPUTS_DIR' in os.environ else None)
    run(parser.parse_args())
