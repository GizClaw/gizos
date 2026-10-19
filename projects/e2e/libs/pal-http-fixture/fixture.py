"""Owned loopback HTTP/HTTPS peer, no public service or production credential."""
import base64
import contextlib
import hashlib
import ipaddress
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import ssl
import subprocess
import threading
import time
import uuid

BODY = bytes((index * 37 + 11) % 251 for index in range(513))


def certificate(directory, name, advertised):
    """Mint a short-lived, test-only root and localhost certificate."""
    root = Path(directory)
    key, cert = root / (name + '.key'), root / (name + '.pem')
    ca_key, ca = root / (name + '-ca.key'), root / (name + '-ca.pem')
    csr = root / (name + '.csr')
    extensions = root / (name + '.ext')
    extensions.write_text('subjectAltName=DNS:localhost,IP:127.0.0.1,IP:' + str(ipaddress.ip_address(advertised)) + '\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n')
    commands = [
        ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(ca_key), '-out', str(ca), '-days', '2', '-subj', '/CN=PAL HTTP E2E ' + name, '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign', '-addext', 'subjectKeyIdentifier=hash', '-addext', 'authorityKeyIdentifier=keyid:always'],
        ['req', '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key), '-out', str(csr), '-subj', '/CN=localhost'],
        ['x509', '-req', '-in', str(csr), '-CA', str(ca), '-CAkey', str(ca_key), '-CAcreateserial', '-out', str(cert), '-days', '2', '-extfile', str(extensions)],
    ]
    for command in commands:
        subprocess.run(['openssl'] + command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    public = subprocess.check_output(['openssl', 'x509', '-in', str(cert), '-pubkey', '-noout'])
    der = subprocess.check_output(['openssl', 'pkey', '-pubin', '-outform', 'DER'], input=public)
    return key, cert, ca, base64.b64encode(hashlib.sha256(der).digest()).decode()



class TLSRejectionLedger:
    """Associate pre-HTTP TLS failures with an explicitly armed peer/run."""
    def __init__(self):
        self.lock = threading.Lock()
        self.next_arm = 0
        self.by_peer = {}
        self.by_run = {}

    def arm(self, peer, run_id):
        with self.lock:
            self.next_arm += 1
            record = dict(arm=self.next_arm, run_id=run_id, events=[], http_requests=0)
            self.by_peer[peer] = record
            self.by_run[run_id] = record

    def accepted(self, peer):
        with self.lock:
            record = self.by_peer.get(peer)
            if record is None:
                return None
            event = dict(client_hello=False, certificate_presented=False,
                         peer_alerts=[], finished=False, handshake_succeeded=False,
                         error=None)
            record['events'].append(event)
            return event

    def message(self, event, direction, content_type, message_type, data):
        if event is None:
            return
        with self.lock:
            if direction == 'read' and int(content_type) == 22 and int(message_type) == 1:
                event['client_hello'] = True
            if direction == 'write' and int(content_type) == 22 and int(message_type) == 11:
                event['certificate_presented'] = True
            if direction == 'read' and int(content_type) == 21 and len(data) == 2:
                event['peer_alerts'].append(int(data[1]))

    def finished(self, event, succeeded, error=None):
        if event is None:
            return
        with self.lock:
            if event['finished']:
                return
            event['finished'] = True
            event['handshake_succeeded'] = succeeded
            event['error'] = error

    def http_request(self, peer):
        with self.lock:
            record = self.by_peer.get(peer)
            if record is not None:
                record['http_requests'] += 1

    def proof(self, run_id, peer=None):
        with self.lock:
            record = self.by_run.get(run_id)
            if record is None or (peer is not None and self.by_peer.get(peer) is not record):
                raise RuntimeError('TLS rejection was not armed for this peer/run')
            value = dict(arm=record['arm'], run_id=record['run_id'],
                         http_requests=record['http_requests'],
                         events=[dict(event, peer_alerts=list(event['peer_alerts'])) for event in record['events']])
        events = value['events']
        if not events or value['http_requests'] or any(not event['finished'] or event['handshake_succeeded'] for event in events):
            raise RuntimeError('untrusted TLS endpoint has no completed rejecting handshake')
        if not any(event['client_hello'] and event['certificate_presented'] for event in events):
            raise RuntimeError('untrusted TLS peer did not present its certificate')
        value['verified'] = True
        return value


class ObservedTLSServer(ThreadingHTTPServer):
    """Keep TLS handshake observation separate from HTTP request arrival."""
    def __init__(self, address, context, ledger):
        super().__init__(address, Handler)
        self.daemon_threads = True
        self.context = context
        self.tls_ledger = ledger
        self.is_untrusted = ledger is not None
        if ledger is not None:
            # Pinned CPython/OpenSSL exposes decoded handshake messages here,
            # including TLS 1.3 Certificate. Missing instrumentation fails closed.
            if not hasattr(context, '_msg_callback'):
                raise RuntimeError('TLS fixture requires SSL handshake message observation')
            context._msg_callback = self.tls_message

    def tls_message(self, connection, direction, version, content_type, message_type, data):
        del version
        self.tls_ledger.message(getattr(connection, 'h2_tls_event', None),
                                direction, content_type, message_type, data)

    def process_request_thread(self, request, client_address):
        event = self.tls_ledger.accepted(client_address[0]) if self.tls_ledger else None
        connection = request
        try:
            request.settimeout(15)
            connection = self.context.wrap_socket(request, server_side=True, do_handshake_on_connect=False)
            connection.h2_tls_event = event
            connection.do_handshake()
            if self.tls_ledger:
                self.tls_ledger.finished(event, True)
            self.finish_request(connection, client_address)
        except (ssl.SSLError, OSError) as error:
            if self.tls_ledger:
                self.tls_ledger.finished(event, False, getattr(error, 'reason', type(error).__name__))
        finally:
            self.shutdown_request(connection)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def response(self, status, payload=BODY, extra=(), chunked=False, delay=False, truncated=False):
        self.send_response(status)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'X-H2-Input, Content-Type')
        self.send_header('Access-Control-Expose-Headers', '*')
        self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')
        self.send_header('X-H2-Method', self.command)
        self.send_header('Connection', 'close')
        self.send_header('Cache-Control', 'no-store')
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
        elif status != 204:
            self.send_header('Content-Length', str(len(payload)))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        self.close_connection = True
        if self.command == 'HEAD' or status == 204:
            return
        if truncated:
            self.wfile.write(payload[:67])
        elif delay:
            self.wfile.write(payload[:17])
            self.wfile.flush()
            time.sleep(4.0)
            self.wfile.write(payload[17:])
        elif chunked:
            for offset in range(0, len(payload), 41):
                part = payload[offset:offset + 41]
                self.wfile.write(('%x\r\n' % len(part)).encode() + part + b'\r\n')
                self.wfile.flush()
            self.wfile.write(b'0\r\n\r\n')
        else:
            self.wfile.write(payload)

    def dispatch(self):
        try:
            if getattr(self.server, 'is_untrusted', False):
                self.server.tls_ledger.http_request(self.client_address[0])
            if self.command == 'OPTIONS' and self.headers.get('Access-Control-Request-Method'):
                self.response(204, b'')
                return
            path = self.path.split('?', 1)[0]
            pieces = path.split('/', 2)
            if len(pieces) != 3 or pieces[1] != self.server.session:
                self.response(404, b'bad-session')
                return
            route = '/' + pieces[2]
            run_id = ''
            prefix = '/' + self.server.session
            if route.startswith('/run/'):
                run_parts = route.split('/', 3)
                if len(run_parts) != 4 or len(run_parts[2]) != 16 or any(c not in '0123456789abcdef' for c in run_parts[2]):
                    self.response(400, b'bad-run')
                    return
                run_id = run_parts[2]
                prefix += '/run/' + run_id
                route = '/' + run_parts[3]
            with self.server.lock:
                counters = self.server.runs.setdefault(run_id, {})
                attempt = counters.get(route, 0) + 1
                counters[route] = attempt
            if route == '/tls-rejection/arm':
                self.server.rejections.arm(self.client_address[0], run_id)
                self.response(200, b'armed')
                return
            if route == '/tls-rejection/proof':
                try:
                    self.server.rejections.proof(run_id, self.client_address[0])
                    self.response(200, b'certificate-presented-and-rejected')
                except RuntimeError:
                    self.response(409, b'TLS rejection not observed')
                return
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 <= length <= 4096:
                self.response(413, b'')
                return
            body = self.rfile.read(length)
            if route == '/slow-headers':
                time.sleep(4.0)
            if route.startswith('/retry/'):
                if route == '/retry/deadline':
                    time.sleep(2.0)
                status = 200 if route == '/retry/recover' and attempt > 1 else 503
                self.response(status, extra=[('X-H2-Attempt', str(attempt))])
            elif route == '/headers':
                value = self.headers.get('X-H2-Input', '')
                self.response(200 if value == 'byte-span' else 400,
                              extra=[('X-H2-Marker', value)])
            elif route == '/truncated':
                self.response(200, truncated=True)
            elif route == '/empty':
                self.response(204, b'')
            elif route.startswith('/status/'):
                self.response(int(route.rsplit('/', 1)[1]))
            elif route == '/echo':
                self.response(200 if body == BODY else 400, body)
            elif route in ('/redirect/relative', '/redirect/303', '/redirect/307'):
                code = route.rsplit('/', 1)[1]
                target = '../bytes' if code == 'relative' else (prefix + ('/echo' if code == '307' else '/bytes'))
                self.response(302 if code == 'relative' else int(code), b'', [('Location', target)])
            elif route in ('/bytes', '/chunked', '/slow-body', '/slow-headers'):
                self.response(200 if self.command in ('GET', 'HEAD') else 405,
                              chunked=route == '/chunked', delay=route == '/slow-body')
            else:
                self.response(404, b'unknown-route')
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = do_OPTIONS = dispatch


class Fixture:
    def __init__(self, directory, bind='127.0.0.1', advertised='127.0.0.1'):
        self.session = uuid.uuid4().hex
        self.servers = []
        self.threads = []
        self.rejections = TLSRejectionLedger()
        self.key, trusted_cert, self.ca, self.spki = certificate(directory, 'trusted', advertised)
        bad_key, bad_cert, _, _ = certificate(directory, 'untrusted', advertised)
        endpoints = []
        for index, (key, cert) in enumerate(((None, None), (self.key, trusted_cert), (bad_key, bad_cert))):
            if cert:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(cert, key)
                context.set_alpn_protocols(['http/1.1'])
                server = ObservedTLSServer((bind, 0), context, self.rejections if index == 2 else None)
            else:
                server = ThreadingHTTPServer((bind, 0), Handler)
            server.daemon_threads = True
            server.session = self.session
            server.runs = {}
            server.lock = threading.Lock()
            server.rejections = self.rejections
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            self.servers.append(server)
            self.threads.append(thread)
            endpoints.append(f'{"https" if cert else "http"}://{advertised}:{server.server_port}/{self.session}')
        self.http, self.https, self.untrusted = endpoints

    def verify_arrivals(self, run_id=''):
        """A connection timeout must not masquerade as a response/retry test."""
        with self.servers[0].lock:
            attempts = self.servers[0].runs.get(run_id, {}).copy()
        expected = {'/slow-headers': 2, '/slow-body': 1, '/retry/recover': 2, '/retry/exhausted': 3}
        for path, count in expected.items():
            if attempts.get(path) != count:
                raise RuntimeError(f'fixture arrival mismatch for {path}: {attempts.get(path)} != {count}')
        if not 2 <= attempts.get('/retry/deadline', 0) <= 5:
            raise RuntimeError('retry deadline did not exercise multiple real attempts')
        return attempts

    def verify_tls_rejection(self, run_id=''):
        return self.rejections.proof(run_id)

    def close(self):
        for server in self.servers:
            server.shutdown()
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)
            if thread.is_alive():
                raise RuntimeError('fixture did not close')

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
