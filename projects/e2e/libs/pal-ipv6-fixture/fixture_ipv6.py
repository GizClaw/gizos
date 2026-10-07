"""Real IPv6 TCP/UDP/TLS and application peers; IPv4 fallback is separately bound."""
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import socket
import socketserver
import threading
from fixture import Fixture

class IPv6Fixture:
    def __init__(self, bind='::1', advertise=None, callback_bridge=None, callback_cleanup=None):
        self.raw = Fixture(bind=bind, advertise=advertise, callback_bridge=callback_bridge,
                           callback_cleanup=callback_cleanup)
        self.records = []
        self.lock = threading.Lock()
        self.servers = []
        self.threads = []
        owner = self
        class HTTP(BaseHTTPRequestHandler):
            def log_message(self, *_): pass
            def do_GET(self):
                prefix = '/ipv6/' if self.server.address_family == socket.AF_INET6 else '/fallback/'
                if self.path != prefix + owner.raw.session:
                    self.send_error(404)
                    return
                body = owner.raw.session.encode('ascii')
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Access-Control-Allow-Origin', '*')
                self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')
                self.end_headers()
                self.wfile.write(body)
                with owner.lock:
                    owner.records.append(dict(protocol='http', family=self.server.address_family,
                        peer=self.client_address[0], payload=body.decode(), path=self.path))
        class HTTP6(ThreadingHTTPServer):
            address_family = socket.AF_INET6
            daemon_threads = True
            def server_bind(self):
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
                super().server_bind()
        class TCP6(socketserver.ThreadingTCPServer):
            address_family = socket.AF_INET6
            daemon_threads = True
            allow_reuse_address = True
            def server_bind(self):
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
                super().server_bind()
        class MQTT(socketserver.StreamRequestHandler):
            def handle(self):
                self.request.settimeout(5)
                def packet():
                    kind = self.rfile.read(1)
                    if not kind: return None, b''
                    size, shift = 0, 0
                    for _ in range(4):
                        byte = self.rfile.read(1)
                        if not byte: raise EOFError('MQTT remaining length')
                        size |= (byte[0] & 127) << shift
                        if not byte[0] & 128: break
                        shift += 7
                    else: raise ValueError('MQTT remaining length')
                    if size > 4096: raise ValueError('MQTT packet capacity')
                    body = self.rfile.read(size)
                    if len(body) != size: raise EOFError('MQTT body')
                    return kind[0] >> 4, body
                kind, connect = packet()
                if kind != 1 or owner.raw.session.encode() not in connect:
                    return
                self.wfile.write(b'\x20\x02\x00\x00'); self.wfile.flush()
                kind, publish = packet()
                if kind != 3: return
                topic_len = int.from_bytes(publish[:2], 'big')
                topic = publish[2:2 + topic_len]
                payload = publish[2 + topic_len:]
                if topic != b'pal/ipv6' or payload != owner.raw.session.encode():
                    return
                kind, _ = packet()
                if kind != 14: raise ValueError('MQTT disconnect missing')
                with owner.lock:
                    owner.records.append(dict(protocol='mqtt', family=socket.AF_INET6,
                        peer=self.client_address[0], payload=payload.decode(), disconnected=True))
        owner.dns_answer = socket.inet_pton(socket.AF_INET6, self.raw.advertise.split('%', 1)[0])
        class UDP6(socketserver.ThreadingUDPServer):
            address_family = socket.AF_INET6
            daemon_threads = True
        class DNS(socketserver.BaseRequestHandler):
            def handle(self):
                packet, transport = self.request
                if len(packet) < 17 or packet[4:6] != b'\x00\x01': return
                offset, labels = 12, []
                while offset < len(packet) and packet[offset]:
                    length = packet[offset]; offset += 1
                    if length > 63 or offset + length >= len(packet): return
                    labels.append(packet[offset:offset + length].decode('ascii')); offset += length
                offset += 1
                if offset + 4 != len(packet) or packet[offset:] != b'\x00\x1c\x00\x01': return
                name = '.'.join(labels)
                if name != owner.raw.session + '.ipv6.test': return
                response = packet[:2] + b'\x85\x80\x00\x01\x00\x01\x00\x00\x00\x00' + packet[12:]
                response += b'\xc0\x0c\x00\x1c\x00\x01\x00\x00\x00\x01\x00\x10' + owner.dns_answer
                with owner.lock:
                    transport.sendto(response, self.client_address)
                    owner.records.append(dict(protocol='dns-aaaa', family=socket.AF_INET6,
                        peer=self.client_address[0], name=name, txid=int.from_bytes(packet[:2], 'big'),
                        answer=socket.inet_ntop(socket.AF_INET6, owner.dns_answer)))
        self.dns = UDP6((bind, 0), DNS)
        self.dns_port = self.dns.server_address[1]
        self.http6 = HTTP6((bind, 0), HTTP)
        self.http4 = ThreadingHTTPServer(('127.0.0.1', 0), HTTP)
        self.mqtt = TCP6((bind, 0), MQTT)
        for server in (self.http6, self.http4, self.mqtt, self.dns):
            self.servers.append(server)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            self.threads.append(worker); worker.start()
        address = self.raw.advertise
        self.http_url = f'http://[{address}]:{self.http6.server_port}/ipv6/{self.raw.session}'
        self.fallback_url = f'http://localhost:{self.http4.server_port}/fallback/{self.raw.session}'
        self.mqtt_port = self.mqtt.server_address[1]
    def snapshot(self):
        with self.lock:
            records = list(self.records)
        return dict(raw=self.raw.snapshot(), applications=records)
    def verify(self, local_fallback=False):
        with self.lock:
            records = list(self.records)
        assert any(r['protocol']=='http' and r['family']==socket.AF_INET6 for r in records), records
        if not local_fallback:
            assert any(r['protocol']=='http' and r['family']==socket.AF_INET for r in records), records
        for record in records:
            if record['protocol'] == 'http':
                assert record['family'] in (socket.AF_INET6, socket.AF_INET), records
                prefix = '/ipv6/' if record['family'] == socket.AF_INET6 else '/fallback/'
                assert record.get('path') == prefix + self.raw.session, records
        assert any(r['protocol']=='mqtt' and r['family']==socket.AF_INET6 for r in records), records
        assert all(r.get('payload', self.raw.session)==self.raw.session for r in records), records
        assert any(r['protocol']=='dns-aaaa' and r['family']==socket.AF_INET6 for r in records), records
    def close(self):
        for server in self.servers:
            server.shutdown(); server.server_close()
        for thread in self.threads:
            thread.join(timeout=5)
            if thread.is_alive(): raise RuntimeError('IPv6 fixture teardown')
        self.raw.close()
    def __enter__(self): return self
    def __exit__(self, *_): self.close()

@contextlib.contextmanager
def pion(binary, bind='::1', advertise=None):
    import re
    import select
    import subprocess
    import time
    from pathlib import Path
    address = advertise or bind
    endpoint = '[' + bind + ']:0'
    process = subprocess.Popen([str(Path(binary).resolve()), '--listen=' + endpoint,
        '--stun-listen=' + endpoint, '--turn-listen=127.0.0.1:0', '--candidate-ip=' + (bind if bind != '::' else address),
        '--advertised-candidate-ip=' + address],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if not select.select([process.stdout], [], [], .1)[0]: continue
            line = process.stdout.readline().decode()
            if not line: raise RuntimeError('IPv6 Pion exited')
            if 'H2_WEBRTC_TEST_SERVER_READY' in line:
                http_endpoint = re.search(r' http=([^ ]+)', line)[1]
                stun_endpoint = re.search(r' stun=([^ ]+)', line)[1]
                http_port = int(http_endpoint.rsplit(':', 1)[1])
                stun_port = int(stun_endpoint.rsplit(':', 1)[1])
                yield dict(offer=f'http://[{address}]:{http_port}/offer',
                    stun=f'stun:[{address}]:{stun_port}', http_port=http_port)
                return
        raise TimeoutError('IPv6 Pion startup')
    finally:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
        process.stdout.close()
