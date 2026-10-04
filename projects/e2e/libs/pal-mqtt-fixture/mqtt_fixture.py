"""Owned MQTT 3.1.1 wire broker for real PAL TCP/TLS integration.

Implements subscriptions, retained delivery and QoS 0/1 acknowledgements.
Faults are selected by the unique client ID case suffix, never by a fake PAL.
"""
import copy
from datetime import datetime, timezone
import math
import socket
import ssl
import struct
import threading
import time
import uuid

from fixture import certificate


def field(value):
    return struct.pack('!H', len(value)) + value


def packet(kind, body=b''):
    remaining = len(body)
    encoded = bytearray([kind])
    while True:
        digit = remaining % 128
        remaining //= 128
        encoded.append(digit | (128 if remaining else 0))
        if not remaining:
            return bytes(encoded) + body


def span(data, offset):
    if offset + 2 > len(data):
        raise ValueError('short MQTT field')
    length = struct.unpack_from('!H', data, offset)[0]
    end = offset + 2 + length
    if end > len(data):
        raise ValueError('short MQTT field bytes')
    return data[offset + 2:end], end


class Peer:
    def __init__(self, broker, connection, peer_ip):
        self.broker = broker
        self.connection = connection
        self.peer_ip = peer_ip
        self.lock = threading.Lock()
        self.filters = {}
        self.client_id = ''
        self.next_id = 1
        self.pending = set()

    def send(self, value):
        with self.lock:
            self.connection.sendall(value)

    def deliver(self, topic, payload, qos, retain=False):
        identifier = b''
        if qos:
            packet_id = self.next_id
            self.next_id = self.next_id % 65535 + 1
            self.pending.add(packet_id)
            identifier = struct.pack('!H', packet_id)
        self.send(packet(0x30 | (qos << 1) | int(retain), field(topic) + identifier + payload))

    def exact(self, length):
        result = bytearray()
        while len(result) < length:
            if self.broker.stopped.is_set():
                raise EOFError
            try:
                value = self.connection.recv(length - len(result))
            except socket.timeout:
                continue
            if not value:
                raise EOFError
            result.extend(value)
        return bytes(result)

    def read(self):
        first = self.exact(1)[0]
        length = 0
        for index in range(4):
            digit = self.exact(1)[0]
            length += (digit & 127) * 128 ** index
            if not digit & 128:
                if length > 65536:
                    raise ValueError('oversized MQTT packet')
                return first, self.exact(length)
        raise ValueError('malformed remaining length')

    def connect(self, body):
        protocol, offset = span(body, 0)
        if protocol != b'MQTT' or body[offset] != 4:
            raise ValueError('expected MQTT 3.1.1')
        flags = body[offset + 1]
        client_id, offset = span(body, offset + 4)
        self.client_id = client_id.decode('ascii')
        if not self.client_id.startswith(self.broker.session + '-') and not (
                self.broker.allow_smoke and self.client_id.endswith('-publish-qos0')):
            raise ValueError('foreign MQTT fixture session')
        username, password = b'', b''
        if flags & 4:
            raise ValueError('will is outside fixture scope')
        if flags & 128:
            username, offset = span(body, offset)
        if flags & 64:
            password, offset = span(body, offset)
        if offset != len(body):
            raise ValueError('extra CONNECT bytes')
        self.broker.record(self.client_id, 'connect')
        if self.client_id.endswith('-tls-trusted'):
            run = self.client_id[:-len('-tls-trusted')]
            with self.broker.lock:
                self.broker.current_run_by_peer[self.peer_ip] = run
            event = getattr(self.connection, 'h2_event', None)
            if event is not None:
                event['run'] = run
        if self.client_id.endswith('-connect-timeout'):
            return
        if self.client_id.endswith(('-authenticated', '-auth-refused')):
            if username != b'fixture' or password != b'fixture-password':
                self.send(packet(0x20, b'\x00\x05'))
                self.broker.record(self.client_id, 'auth-refused')
                raise EOFError
        self.send(packet(0x20, b'\x00\x00'))
        if self.client_id.endswith('-remote-disconnect'):
            raise EOFError

    def serve(self):
        kind, body = self.read()
        if kind != 0x10:
            raise ValueError('first packet is not CONNECT')
        self.connect(body)
        while True:
            kind, body = self.read()
            operation = kind >> 4
            if operation == 8:
                if kind != 0x82 or len(body) < 2:
                    raise ValueError('bad SUBSCRIBE')
                codes = bytearray()
                offset = 2
                retained = []
                while offset < len(body):
                    topic, offset = span(body, offset)
                    qos = body[offset]
                    offset += 1
                    if not topic or qos not in (0, 1):
                        raise ValueError('bad topic or QoS')
                    self.filters[topic] = qos
                    codes.append(qos)
                    with self.broker.lock:
                        if topic in self.broker.retained:
                            payload, published_qos = self.broker.retained[topic]
                            retained.append((topic, payload, min(qos, published_qos)))
                self.send(packet(0x90, body[:2] + codes))
                self.broker.record(self.client_id, 'subscribe')
                for topic, payload, qos in retained:
                    self.deliver(topic, payload, qos, retain=True)
            elif operation == 3:
                topic, offset = span(body, 0)
                qos = (kind >> 1) & 3
                if qos not in (0, 1):
                    raise ValueError('unsupported QoS')
                identifier = body[offset:offset + 2] if qos else b''
                offset += 2 if qos else 0
                payload = body[offset:]
                with self.broker.lock:
                    if kind & 1:
                        if payload:
                            self.broker.retained[topic] = (payload, qos)
                        else:
                            self.broker.retained.pop(topic, None)
                    peers = list(self.broker.peers)
                if qos:
                    self.send(packet(0x40, identifier))
                for peer in peers:
                    if topic in peer.filters:
                        peer.deliver(topic, payload, min(qos, peer.filters[topic]))
                self.broker.record(self.client_id, 'publish')
            elif operation == 10:
                offset = 2
                while offset < len(body):
                    topic, offset = span(body, offset)
                    self.filters.pop(topic, None)
                self.send(packet(0xB0, body[:2]))
                self.broker.record(self.client_id, 'unsubscribe')
            elif operation == 4:
                identifier = struct.unpack('!H', body)[0]
                if identifier not in self.pending:
                    raise ValueError('unexpected PUBACK')
                self.pending.remove(identifier)
                self.broker.record(self.client_id, 'incoming-puback')
            elif operation == 12:
                self.broker.record(self.client_id, 'ping')
                if not self.client_id.endswith('-keepalive-timeout'):
                    self.send(packet(0xD0))
            elif operation == 14:
                self.broker.record(self.client_id, 'disconnect')
                return
            else:
                raise ValueError('unexpected MQTT operation ' + str(operation))


class Broker:
    def __init__(self, session, context=None, allow_smoke=False, bind='127.0.0.1', tls_handshake_timeout=2):
        if not math.isfinite(tls_handshake_timeout) or not 0 < tls_handshake_timeout <= 60:
            raise ValueError('TLS handshake timeout must be in (0, 60] seconds')
        self.session = session
        self.context = context
        self.tls_handshake_timeout = tls_handshake_timeout
        self.allow_smoke = allow_smoke
        self.listener = socket.socket()
        self.listener.bind((bind, 0))
        self.listener.listen(32)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.lock = threading.Lock()
        self.stopped = threading.Event()
        self.peers = set()
        self.threads = []
        self.retained = {}
        self.arrivals = {}
        self.failures = []
        self.handshakes = []
        self.current_run_by_peer = {}
        if context:
            context._msg_callback = self.tls_message
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def tls_message(self, connection, direction, version, content_type, message_type, data):
        event = getattr(connection, 'h2_event', None)
        if event is not None:
            with self.lock:
                message = dict(direction=direction, version=int(version),
                    content_type=int(content_type), message_type=int(message_type),
                    elapsed_ms=int((time.monotonic() - event['_started_monotonic']) * 1000),
                    data_length=len(data))
                if int(content_type) == 256 and len(data) == 5:
                    message['record_length'] = int.from_bytes(data[3:5], 'big')
                if int(content_type) == 21 and len(data) == 2:
                    message.update(alert_level=data[0], alert_description=data[1])
                event['messages'].append(message)
                if direction == 'read' and int(content_type) == 22 and int(message_type) == 1:
                    event['client_hello'] = True
                if direction == 'write' and int(content_type) == 22 and int(message_type) == 11:
                    event['certificate_presented'] = True

    def handshake_snapshot(self):
        with self.lock:
            return [{key:copy.deepcopy(value) for key,value in event.items() if not key.startswith('_')}
                    for event in self.handshakes]

    def diagnostic_snapshot(self):
        with self.lock:
            return dict(failures=self.failures.copy(), active_clients=len(self.peers),
                        client_ids=sorted(peer.client_id for peer in self.peers),
                        arrivals=copy.deepcopy(self.arrivals))

    def record(self, client_id, operation):
        with self.lock:
            row = self.arrivals.setdefault(client_id, {})
            row[operation] = row.get(operation, 0) + 1

    def accept(self):
        while not self.stopped.is_set():
            try:
                connection, address = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            thread = threading.Thread(target=self.handle, args=(connection, address[0], address[1]), daemon=True)
            self.threads.append(thread)
            thread.start()

    def handle(self, connection, peer_ip, peer_port):
        peer = None
        try:
            connection.settimeout(0.1)
            if self.context:
                event = dict(client_hello=False, certificate_presented=False, succeeded=False, finished=False,
                             peer_ip=peer_ip, peer_port=peer_port, run=self.current_run_by_peer.get(peer_ip),
                             timeout_seconds=self.tls_handshake_timeout, messages=[],
                             started_at_utc=datetime.now(timezone.utc).isoformat(),
                             _started_monotonic=time.monotonic())
                with self.lock:
                    self.handshakes.append(event)
                connection = self.context.wrap_socket(connection, server_side=True, do_handshake_on_connect=False)
                connection.h2_event = event
                connection.settimeout(self.tls_handshake_timeout)
                try:
                    connection.do_handshake()
                    with self.lock:
                        event.update(succeeded=True, tls_version=connection.version(), cipher=connection.cipher())
                except (ssl.SSLError, OSError) as error:
                    with self.lock:
                        event['error'] = dict(type=type(error).__name__, message=str(error),
                            errno=error.errno, library=getattr(error, 'library', None),
                            reason=getattr(error, 'reason', None))
                    return
                finally:
                    with self.lock:
                        event.update(finished=True, elapsed_ms=int((time.monotonic() - event['_started_monotonic']) * 1000))
                connection.settimeout(0.1)
            peer = Peer(self, connection, peer_ip)
            with self.lock:
                self.peers.add(peer)
            peer.serve()
        except (EOFError, ConnectionError, BrokenPipeError):
            pass
        except (ValueError, IndexError, struct.error, OSError) as error:
            with self.lock:
                self.failures.append(type(error).__name__ + ': ' + str(error))
        finally:
            if peer:
                with self.lock:
                    self.peers.discard(peer)
            connection.close()

    def close(self):
        self.stopped.set()
        self.listener.close()
        self.thread.join(timeout=2)
        for thread in self.threads:
            thread.join(timeout=3)
        if self.thread.is_alive() or any(thread.is_alive() for thread in self.threads):
            raise RuntimeError('MQTT fixture thread did not stop')


class Fixture:
    def __init__(self, directory, allow_smoke=False, bind='127.0.0.1', advertised='127.0.0.1', tls_handshake_timeout=2):
        if not math.isfinite(tls_handshake_timeout) or not 0 < tls_handshake_timeout <= 60:
            raise ValueError('TLS handshake timeout must be in (0, 60] seconds')
        self.session = uuid.uuid4().hex
        self.advertised = advertised
        key, cert, self.ca, _ = certificate(directory, 'mqtt-trusted', advertised)
        _, _, self.wrong_ca, _ = certificate(directory, 'mqtt-other-root', advertised)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        # This suite measures client cleanup, not process-wide session caching.
        # MQTT/TLS resumption is outside its declared qualification scope.
        context.options |= ssl.OP_NO_TICKET
        context.num_tickets = 0
        context.load_cert_chain(cert, key)
        self.tcp = Broker(self.session, allow_smoke=allow_smoke, bind=bind)
        self.tls = Broker(self.session, context, allow_smoke=allow_smoke, bind=bind,
                          tls_handshake_timeout=tls_handshake_timeout)

    def __enter__(self):
        return self

    def __exit__(self, *ignored):
        self.tcp.close()
        self.tls.close()

    def verify(self, full=True, session=None):
        run_session = self.session if session is None else session
        deadline = time.monotonic() + 2
        while (self.tcp.peers or self.tls.peers) and time.monotonic() < deadline:
            time.sleep(0.01)
        if self.tcp.peers or self.tls.peers or self.tcp.retained or self.tls.retained:
            raise RuntimeError('broker connection or retained payload leaked')
        if self.tcp.failures or self.tls.failures:
            raise RuntimeError('broker protocol failures: ' + repr(self.tcp.failures + self.tls.failures))
        if full:
            rejected = [event for event in self.tls.handshakes if not event['succeeded'] and event['run'] == run_session]
            if len(rejected) != 2 or any(not event['finished'] or not event['client_hello'] or
                                       not event['certificate_presented'] for event in rejected):
                raise RuntimeError('TLS rejection lacks two real certificate handshakes')
            required = {'publish-qos0': 'publish', 'publish-qos1': 'incoming-puback',
                        'authenticated': 'connect', 'auth-refused': 'auth-refused',
                        'connect-timeout': 'connect', 'unsubscribe-ack': 'unsubscribe',
                        'keepalive-timeout': 'ping', 'remote-disconnect': 'connect'}
            for case, operation in required.items():
                if not self.tcp.arrivals.get(run_session + '-' + case, {}).get(operation):
                    raise RuntimeError('missing broker witness: ' + case + '/' + operation)
            ordinary = ['connect-events', 'repeated-connect', 'subscribe-qos0', 'subscribe-qos1',
                        'subscribe-multi', 'publish-qos0', 'publish-qos1', 'publish-binary',
                        'publish-empty', 'publish-large', 'input-lifetime', 'unsubscribe-ack',
                        'retained-delivery', 'idle-process', 'invalid-qos', 'invalid-publish',
                        'invalid-subscribe', 'invalid-unsubscribe', 'local-disconnect',
                        'reconnect', 'authenticated', 'qos-capacity', 'repeated-lifecycle']
            for case in ordinary:
                row = self.tcp.arrivals.get(run_session + '-' + case, {})
                if not row.get('connect') or row.get('connect') != row.get('disconnect'):
                    raise RuntimeError('missing balanced wire lifecycle: ' + case)
            retained_row = self.tcp.arrivals[run_session + '-retained-delivery']
            if retained_row.get('connect') != 1 or retained_row.get('publish') != 2 or retained_row.get('disconnect') != 1:
                raise RuntimeError('retained clear requires two real publishes and a completed disconnect')
            for case in ['subscribe-qos0', 'subscribe-qos1', 'subscribe-multi', 'publish-qos0',
                         'publish-qos1', 'publish-binary', 'publish-empty', 'publish-large',
                         'input-lifetime', 'unsubscribe-ack', 'retained-delivery', 'reconnect',
                         'callback-close', 'repeated-lifecycle']:
                if not self.tcp.arrivals.get(run_session + '-' + case, {}).get('subscribe'):
                    raise RuntimeError('missing subscription wire witness: ' + case)
            tls_row = self.tls.arrivals.get(run_session + '-tls-trusted', {})
            if tls_row.get('connect') != 1 or tls_row.get('publish') != 1 or tls_row.get('disconnect') != 1:
                raise RuntimeError('trusted TLS round trip has no broker witness')
            lifecycle = self.tcp.arrivals[run_session + '-repeated-lifecycle']
            if lifecycle.get('connect') != 21 or lifecycle.get('disconnect') != 21 or lifecycle.get('publish') != 20:
                raise RuntimeError('repeated lifecycle count differs from wire evidence')
        return dict(session=run_session,
                    tcp_arrivals={key:value for key,value in self.tcp.arrivals.items() if key.startswith(run_session + '-')},
                    tls_arrivals={key:value for key,value in self.tls.arrivals.items() if key.startswith(run_session + '-')},
                    tls_handshakes=[event for event in self.tls.handshake_snapshot() if event['run'] == run_session],
                    active_clients=0, retained_messages=0)
