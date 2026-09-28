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
        self.key, trusted_cert, self.ca, self.spki = certificate(directory, 'trusted', advertised)
        bad_key, bad_cert, _, _ = certificate(directory, 'untrusted', advertised)
        endpoints = []
        for key, cert in ((None, None), (self.key, trusted_cert), (bad_key, bad_cert)):
            server = ThreadingHTTPServer((bind, 0), Handler)
            server.daemon_threads = True
            server.session = self.session
            server.runs = {}
            server.lock = threading.Lock()
            if cert:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(cert, key)
                context.set_alpn_protocols(['http/1.1'])
                server.socket = context.wrap_socket(server.socket, server_side=True)
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
