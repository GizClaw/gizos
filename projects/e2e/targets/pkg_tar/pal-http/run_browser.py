"""Production Web PAL in real Chromium with narrowly pinned fixture TLS."""
import argparse
from http.server import ThreadingHTTPServer
import json
import hashlib
import os
from pathlib import Path
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlencode
from fixture import Fixture

root = Path(__file__).absolute().parents[5]
sys.path.insert(0, str(root / 'tools/bazel'))
sys.path.insert(0, str(root / 'projects/e2e/targets/cc_binary/pal-http'))
from web_archive_browser_test import Cdp, find_browser
from web_archive_server import prepared_archive, make_handler, read_header_policy
from run_desktop import validate
from browser_evidence import WorkerEvidenceError, validate_worker_state, validate_certificate_error


def run(args, endpoint_fault=None, worker_fault=None):
    with tempfile.TemporaryDirectory(prefix='h2-http-browser-') as temp, \
            prepared_archive(Path(args.archive).resolve()) as archive, Fixture(temp) as fixture, socket.socket() as refused:
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(archive, read_header_policy(archive)))
        serving = threading.Thread(target=server.serve_forever, daemon=True)
        serving.start()
        refused.bind(('127.0.0.1', 0))
        untrusted = fixture.untrusted
        if endpoint_fault == 'malformed': untrusted = 'https://[invalid'
        if endpoint_fault == 'refused': untrusted = f'https://127.0.0.1:{refused.getsockname()[1]}/{fixture.session}'
        url = f'http://127.0.0.1:{server.server_port}/?' + urlencode(dict(http=fixture.http, https=fixture.https, untrusted=untrusted, workerProbeFault=worker_fault or ''))
        to_read, to_write = os.pipe()
        from_read, from_write = os.pipe()

        def pipes():
            reader, writer = os.dup(to_read), os.dup(from_write)
            os.dup2(reader, 3)
            os.dup2(writer, 4)

        events = queue.Queue()
        process = subprocess.Popen([str(find_browser()), '--headless', '--no-sandbox',
                                    '--remote-debugging-pipe', '--ignore-certificate-errors-spki-list=' + fixture.spki,
                                    '--user-data-dir=' + temp + '/profile', 'about:blank'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   preexec_fn=pipes, pass_fds=(3, 4))
        os.close(to_read)
        os.close(from_write)
        cdp = Cdp(to_write, from_read, events)
        lines = []
        network_requests = {}
        network_failures = []
        try:
            target = cdp.send('Target.createTarget', {'url': 'about:blank'})['targetId']
            session = cdp.send('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']
            cdp.send('Runtime.enable', session=session)
            cdp.send('Page.enable', session=session)
            cdp.send('Network.enable', session=session)
            cdp.send('Target.setAutoAttach', {'autoAttach': True, 'waitForDebuggerOnStart': True, 'flatten': True}, session=session)
            cdp.send('Page.navigate', {'url': url}, session=session)
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                try:
                    event = events.get(timeout=0.2)
                except queue.Empty:
                    if process.poll() is not None:
                        raise RuntimeError('Chromium exited early')
                    continue
                method = event.get('method')
                params = event.get('params', {})
                event_session = event.get('sessionId', session)
                if method == 'Target.attachedToTarget':
                    child = params['sessionId']
                    cdp.send('Network.enable', session=child)
                    cdp.send('Runtime.runIfWaitingForDebugger', session=child)
                    continue
                if method == 'Network.requestWillBeSent':
                    network_requests[(event_session, params['requestId'])] = params['request']['url']
                if method == 'Network.loadingFailed':
                    network_failures.append(dict(url=network_requests.get((event_session, params['requestId'])), error=params.get('errorText')))
                if event.get('method') == 'Runtime.exceptionThrown':
                    raise RuntimeError(event['params'])
                if event.get('method') != 'Runtime.consoleAPICalled' or event_session != session:
                    continue
                text = ' '.join(str(arg.get('value', arg.get('description', ''))) for arg in event['params'].get('args', []))
                print(text, flush=True)
                lines.append(text)
                if 'Aborted(' in text:
                    raise RuntimeError('WASM aborted')
                if text.startswith('H2_PAL_HTTP_SUMMARY '):
                    if endpoint_fault:
                        rows = [json.loads(line.split(' ', 1)[1]) for line in lines if line.startswith('H2_PAL_HTTP_CASE ')]
                        failed = [row['id'] for row in rows if row['status'] != 'PASS']
                        if failed != ['https-untrusted']:
                            raise RuntimeError('bad endpoint did not fail specifically in its TLS case')
                        try:
                            fixture.verify_tls_rejection()
                        except RuntimeError:
                            print('Rejected untrusted browser endpoint:', endpoint_fault)
                            return
                        raise RuntimeError('bad browser endpoint reused a TLS rejection proof')
                    evidence = validate('\n'.join(lines), args.cases)
                    evidence['platform'] = 'wasm-chromium'
                    evidence['tls_verification'] = 'only fixture SPKI pinned; distinct untrusted SPKI rejected'
                    try:
                        state = validate_worker_state(lines)
                    except WorkerEvidenceError:
                        if worker_fault:
                            print('Rejected invalid actual Worker state:', worker_fault)
                            return
                        raise
                    if worker_fault:
                        raise RuntimeError('invalid Worker observation unexpectedly qualified')
                    isolated = cdp.send('Runtime.evaluate', {'expression': 'crossOriginIsolated === true', 'returnByValue': True}, session=session)['result']['value']
                    if isolated is not True:
                        raise RuntimeError('browser was not cross-origin isolated')
                    evidence['browser_state'] = dict(isolated=True, worker=state)
                    evidence['browser_certificate_errors'] = validate_certificate_error(network_failures, untrusted + '/bytes')
                    evidence['tls_rejection'] = fixture.verify_tls_rejection()
                    evidence['artifact_sha256'] = hashlib.sha256(Path(args.archive).read_bytes()).hexdigest()
                    evidence['registry_sha256'] = hashlib.sha256(Path(args.cases).read_bytes()).hexdigest()
                    evidence['fixture_attempts'] = fixture.verify_arrivals()
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
    parser.add_argument('--negative-workers', action='store_true')
    parser.add_argument('--negative-endpoints', action='store_true')
    parser.add_argument('--evidence', default=str(Path(os.environ['TEST_UNDECLARED_OUTPUTS_DIR']) / 'qualified.json') if 'TEST_UNDECLARED_OUTPUTS_DIR' in os.environ else None)
    args = parser.parse_args()
    if args.negative_workers:
        for fault in ['missing', 'invalid-count', 'retained']:
            run(args, worker_fault=fault)
    elif args.negative_endpoints:
        for fault in ['malformed', 'refused']:
            run(args, endpoint_fault=fault)
    else:
        run(args)
