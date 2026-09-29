"""Owned raw TCP/UDP/TLS peers with run-local, fail-closed handshake evidence."""
import contextlib
import copy
import json
from pathlib import Path
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time
import uuid

HEADER_BYTES = 96
PAYLOAD_BYTES = 4097
SERVER_NAME = "pal-net-tls.test"
ALPN = "h2-pal-e2e"


def payload(length=PAYLOAD_BYTES):
    return bytes((index * 37 + 11) % 251 for index in range(length))


def packet(session, case, length=PAYLOAD_BYTES):
    header = f"H2NETTLS/1 {session} {case}".encode()
    if len(header) >= HEADER_BYTES:
        raise ValueError("case exceeds wire header")
    return header.ljust(HEADER_BYTES, b"\0") + payload(length)


def mint(directory):
    root = Path(directory)
    ca, key = root / "root.pem", root / "root.key"
    leaf, leaf_key, csr = root / "leaf.pem", root / "leaf.key", root / "leaf.csr"
    wrong = root / "wrong.pem"
    expired = root / "expired.pem"
    extension = root / "leaf.ext"
    extension.write_text("[leaf]\nsubjectAltName=DNS:" + SERVER_NAME +
        "\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment"
        "\nextendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n")
    def openssl(*args):
        subprocess.run(["openssl", *map(str, args)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for name, target in (("PAL Net TLS E2E", ca), ("PAL Net TLS Wrong Root", wrong)):
        target_key = key if target == ca else root / "wrong.key"
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", target_key,
                "-out", target, "-days", "2", "-subj", "/CN=" + name,
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    openssl("req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", leaf_key,
            "-out", csr, "-subj", "/CN=" + SERVER_NAME)
    openssl("x509", "-req", "-in", csr, "-CA", ca, "-CAkey", key, "-CAcreateserial",
            "-out", leaf, "-days", "2", "-extfile", extension, "-extensions", "leaf")
    (root / "index").write_text("")
    (root / "serial").write_text("10\n")
    config = root / "ca.conf"
    config.write_text(f"[ca]\ndefault_ca=local\n[local]\ndatabase={root / 'index'}\n"
        f"serial={root / 'serial'}\nnew_certs_dir={root}\ncertificate={ca}\nprivate_key={key}\n"
        "default_md=sha256\ndefault_days=2\npolicy=policy\nunique_subject=no\n"
        "[policy]\ncommonName=supplied\n")
    openssl("ca", "-batch", "-config", config, "-in", csr, "-out", expired,
            "-startdate", "20200101000000Z", "-enddate", "20200102000000Z",
            "-extfile", extension, "-extensions", "leaf", "-notext")
    return ca, wrong, leaf, expired, leaf_key


class Fixture:
    """Only explicit binding may expose the peer to physical device Wi-Fi."""
    def __init__(self, bind="127.0.0.1", advertise=None):
        self.bind = bind
        self.advertise = advertise or bind
        self.session = uuid.uuid4().hex
        self.temp = tempfile.TemporaryDirectory(prefix="h2-net-tls-")
        self.ca, self.wrong_ca, self.cert, self.expired, self.key = mint(self.temp.name)
        self.lock = threading.Lock()
        self.records = {}
        self.history = []
        self.sockets = set()
        self.workers = []
        self.stop = threading.Event()
        self.contexts = {}
        for mode, cert in ((2, self.cert), (3, self.expired)):
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
            ctx.load_cert_chain(cert, self.key)
            ctx.set_alpn_protocols([ALPN])
            if not hasattr(ctx, "_msg_callback"):
                raise RuntimeError("TLS evidence requires decoded SSL message callback")
            ctx._msg_callback = self.message
            ctx.set_servername_callback(self.sni)
            self.contexts[mode] = ctx
        owner = self
        class Server(socketserver.ThreadingTCPServer):
            daemon_threads = True
            allow_reuse_address = True
        class Control(socketserver.StreamRequestHandler):
            def handle(self):
                self.request.settimeout(3)
                raw = self.rfile.readline(1024)
                try:
                    parts = raw.decode("ascii").strip().split()
                    if len(parts) < 4 or parts[1] != owner.session:
                        raise ValueError("session")
                    action, _, run_id, case, *args = parts
                    if len(run_id) != 16 or any(c not in "0123456789abcdef" for c in run_id):
                        raise ValueError("run")
                    if not case or len(case) > 50 or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in case):
                        raise ValueError("case")
                    if action == "ARM" and len(args) == 2:
                        port = owner.arm(case, int(args[0]), int(args[1]), self.client_address[0], run_id=run_id)
                        reply = f"OK {port}\n"
                    elif action == "PROOF" and len(args) == 1:
                        owner.proof(case, int(args[0]), timeout=2, run_id=run_id)
                        reply = "OK 0\n"
                    else:
                        raise ValueError("operation")
                except (ValueError, RuntimeError, KeyError):
                    reply = "FAIL 0\n"
                self.wfile.write(reply.encode("ascii"))
        self.control = Server((bind, 0), Control)
        self.port = self.control.server_address[1]
        self.spawn(self.control.serve_forever)

    def spawn(self, target, *args):
        thread = threading.Thread(target=target, args=args, daemon=True)
        self.workers.append(thread)
        thread.start()
        return thread

    def owned(self, udp=False):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.bind((self.bind, 0))
        with self.lock:
            self.sockets.add(sock)
        return sock

    def arm(self, case, mode, callback, peer, run_id=None):
        run_id = run_id or self.session[:16]
        key = case if run_id == self.session[:16] else run_id + ":" + case
        if mode not in range(7) or (mode == 6) != bool(callback):
            raise ValueError("fixture mode")
        record = dict(session=self.session, run_id=run_id, case=case, mode=mode, peer=peer,
            port=0, accepted=False, client_hello=False, certificate_presented=False,
            peer_alerts=[], sni=None, alpn=None, handshake_succeeded=False,
            finished=False, payload_received=0, payload_sent=0, payload_valid=False,
            error=None, closed=False)
        with self.lock:
            previous = self.records.get(key)
            if previous:
                self.history.append(copy.deepcopy(previous))
            self.records[key] = record
        if mode == 6:
            self.spawn(self.callback, record, callback)
            return callback
        sock = self.owned(udp=mode == 1)
        port = sock.getsockname()[1]
        record['port'] = port
        if mode == 1:
            self.spawn(self.udp, sock, record)
        else:
            sock.listen(1)
            self.spawn(self.tcp, sock, record)
        return port

    def message(self, connection, direction, version, content_type, message_type, data):
        del version
        record = getattr(connection, "h2_record", None)
        if record is None:
            return
        with self.lock:
            if direction == "read" and int(content_type) == 22 and int(message_type) == 1:
                record['client_hello'] = True
            if direction == "write" and int(content_type) == 22 and int(message_type) == 11:
                record['certificate_presented'] = True
            if direction == "read" and int(content_type) == 21 and len(data) == 2:
                record['peer_alerts'].append(int(data[1]))

    def sni(self, connection, name, context):
        del context
        record = getattr(connection, "h2_record", None)
        if record is not None:
            with self.lock:
                record['sni'] = name

    def tcp(self, listener, record):
        connection = None
        try:
            connection, peer = listener.accept()
            if peer[0] != record['peer']:
                raise RuntimeError("different peer")
            record['accepted'] = True
            connection.settimeout(5)
            if record['mode'] == 5:
                record['closed'] = True
                return
            if record['mode'] == 4:
                # Peek without consuming a raw peer's future payload. TLS timeout
                # evidence requires a real ClientHello to this exact silent port.
                try:
                    raw = connection.recv(64)
                    record['client_hello'] = len(raw) > 5 and raw[0] == 22 and raw[5] == 1
                except socket.timeout:
                    pass
                self.stop.wait(0.2)
                return
            if record['mode'] in self.contexts:
                connection = self.contexts[record['mode']].wrap_socket(connection,
                    server_side=True, do_handshake_on_connect=False)
                connection.h2_record = record
                connection.do_handshake()
                record['handshake_succeeded'] = True
                record['alpn'] = connection.selected_alpn_protocol()
            self.echo(connection, record)
        except (OSError, ssl.SSLError, RuntimeError) as error:
            record['error'] = getattr(error, 'reason', type(error).__name__)
        finally:
            if connection is not None:
                with contextlib.suppress(OSError):
                    connection.close()
            with contextlib.suppress(OSError):
                listener.close()
            record['finished'] = True

    def echo(self, connection, record):
        expected = packet(self.session, record['case'])
        request = bytearray()
        while len(request) < len(expected):
            part = connection.recv(min(509, len(expected) - len(request)))
            if not part:
                break
            request.extend(part)
        record['payload_received'] = len(request)
        record['payload_valid'] = bytes(request) == expected
        if not record['payload_valid']:
            raise RuntimeError("request byte mismatch")
        response = bytes(value ^ 0xa5 for value in payload())
        for offset in range(0, len(response), 73):
            connection.sendall(response[offset:offset + 73])
            record['payload_sent'] += len(response[offset:offset + 73])

    def callback(self, record, port):
        try:
            with socket.create_connection((record['peer'], port), timeout=3) as connection:
                record['accepted'] = True
                self.echo(connection, record)
        except (OSError, RuntimeError) as error:
            record['error'] = type(error).__name__
        finally:
            record['finished'] = True

    def udp(self, sock, record):
        try:
            request, peer = sock.recvfrom(4096)
            if peer[0] != record['peer']:
                raise RuntimeError("different peer")
            record['accepted'] = True
            record['payload_received'] = len(request)
            record['payload_valid'] = request == packet(self.session, record['case'], 513)
            if not record['payload_valid']:
                raise RuntimeError("UDP request byte mismatch")
            response = bytes(value ^ 0xa5 for value in payload(513))
            record['payload_sent'] = sock.sendto(response, peer)
        except (OSError, RuntimeError) as error:
            record['error'] = type(error).__name__
        finally:
            sock.close()
            record['finished'] = True

    def proof(self, case, proof, timeout=0, run_id=None):
        run_id = run_id or self.session[:16]
        key = case if run_id == self.session[:16] else run_id + ":" + case
        deadline = time.monotonic() + timeout
        while True:
            with self.lock:
                record = copy.deepcopy(self.records.get(key))
            if record is None or record['session'] != self.session or record['run_id'] != run_id:
                raise RuntimeError("case not armed")
            valid = record['accepted']
            if proof in (0, 2):
                expected = 513 if record['mode'] == 1 else PAYLOAD_BYTES
                valid = valid and record['payload_valid'] and record['payload_received'] == expected + HEADER_BYTES and record['payload_sent'] == expected
                if record['mode'] in (2, 3):
                    valid = valid and record['handshake_succeeded']
                if proof == 2:
                    valid = valid and record['sni'] == SERVER_NAME and record['alpn'] == ALPN
            elif proof == 1:
                valid = valid and record['finished'] and record['client_hello'] and record['certificate_presented'] and not record['handshake_succeeded'] and record['payload_received'] == 0
            elif proof == 3:
                valid = valid and record['mode'] == 4 and record['client_hello'] and not record['handshake_succeeded']
            elif proof == 4:
                valid = valid and record['closed'] and record['finished']
            elif proof == 5:
                valid = valid and record['accepted']
            else:
                raise ValueError("proof")
            if valid:
                return record
            if time.monotonic() >= deadline:
                raise RuntimeError("required peer evidence missing")
            time.sleep(0.01)

    def snapshot(self):
        with self.lock:
            return dict(session=self.session, host=self.advertise, control_port=self.port,
                records=copy.deepcopy(self.history + list(self.records.values())))

    def close(self):
        self.stop.set()
        self.control.shutdown()
        self.control.server_close()
        with self.lock:
            sockets = list(self.sockets)
        for sock in sockets:
            with contextlib.suppress(OSError):
                sock.close()
        for thread in self.workers:
            thread.join(timeout=6)
        self.temp.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
