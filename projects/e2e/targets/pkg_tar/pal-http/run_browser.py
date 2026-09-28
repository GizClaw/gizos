"""Production Web PAL in real Chromium with narrowly pinned fixture TLS."""
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
from fixture import Fixture

root = Path(__file__).absolute().parents[5]
sys.path.insert(0, str(root / 'tools/bazel'))
sys.path.insert(0, str(root / 'projects/e2e/targets/cc_binary/pal-http'))
from web_archive_browser_test import Cdp, find_browser
from web_archive_server import prepared_archive, make_handler, read_header_policy
from run_desktop import validate


def run(args):
    with tempfile.TemporaryDirectory(prefix='h2-http-browser-') as temp, \
            prepared_archive(Path(args.archive).resolve()) as archive, Fixture(temp) as fixture:
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(archive, read_header_policy(archive)))
        serving = threading.Thread(target=server.serve_forever, daemon=True)
        serving.start()
        url = f'http://127.0.0.1:{server.server_port}/?' + urlencode(dict(http=fixture.http, https=fixture.https, untrusted=fixture.untrusted))
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
        try:
            target = cdp.send('Target.createTarget', {'url': 'about:blank'})['targetId']
            session = cdp.send('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']
            cdp.send('Runtime.enable', session=session)
            cdp.send('Page.enable', session=session)
            cdp.send('Page.navigate', {'url': url}, session=session)
            deadline = time.monotonic() + 90
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
                if text.startswith('H2_PAL_HTTP_SUMMARY '):
                    evidence = validate('\n'.join(lines), args.cases)
                    evidence['platform'] = 'wasm-chromium'
                    evidence['tls_verification'] = 'only fixture SPKI pinned; distinct untrusted SPKI rejected'
                    state = cdp.send('Runtime.evaluate', {'expression': '({isolated:crossOriginIsolated,pending:Module.h2WebHttp?.size||0})', 'returnByValue': True}, session=session)['result']['value']
                    if state != {'isolated': True, 'pending': 0}:
                        raise RuntimeError('browser request resources retained: ' + repr(state))
                    evidence['browser_state'] = state
                    evidence['artifact_sha256'] = hashlib.sha256(Path(args.archive).read_bytes()).hexdigest()
                    evidence['registry_sha256'] = hashlib.sha256(Path(args.cases).read_bytes()).hexdigest()
                    evidence['fixture_attempts'] = fixture.servers[0].attempts.copy()
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
    parser.add_argument('--evidence', default=str(Path(os.environ['TEST_UNDECLARED_OUTPUTS_DIR']) / 'qualified.json') if 'TEST_UNDECLARED_OUTPUTS_DIR' in os.environ else None)
    run(parser.parse_args())
