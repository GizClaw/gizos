"""Real wire evidence must be published before the verifier can snapshot it."""
import errno
import http.client
import socket
import struct
import threading
import unittest

from fixture_ipv6 import IPv6Fixture


class VerifierObservedLock:
    """Observe the verifier's acquisition decision without a timing sleep."""
    def __init__(self, lock):
        self.lock = lock
        self.verifier = None
        self.acquired = None
        self.decided = threading.Event()

    def __enter__(self):
        if threading.current_thread() is self.verifier:
            self.acquired = self.lock.acquire(blocking=False)
            self.decided.set()
            if not self.acquired:
                self.lock.acquire()
        else:
            self.lock.acquire()
        return self

    def __exit__(self, *_):
        self.lock.release()


class ObservedDatagramSocket:
    def __init__(self, actual, fail=False):
        self.actual = actual
        self.fail = fail
        self.sent = threading.Event()
        self.release = threading.Event()

    def __getattr__(self, name):
        return getattr(self.actual, name)

    def sendto(self, packet, peer):
        if self.fail:
            raise OSError(errno.EIO, "controlled DNS send failure")
        result = self.actual.sendto(packet, peer)
        self.sent.set()
        if not self.release.wait(5):
            raise RuntimeError("test DNS publication gate was not released")
        return result


class DNSPublicationTest(unittest.TestCase):
    def setUp(self):
        self.fixture = IPv6Fixture()
        self.addCleanup(self.fixture.close)
        self.done = threading.Event()
        self.errors = []
        self.handler = None
        original = self.fixture.dns.finish_request

        def finish(request, peer):
            self.handler = threading.current_thread()
            try:
                original(request, peer)
            except Exception as error:
                self.errors.append(error)
                raise
            finally:
                self.done.set()

        self.fixture.dns.finish_request = finish
        self.fixture.dns.handle_error = lambda *_: None
        self.transport = ObservedDatagramSocket(self.fixture.dns.socket)
        self.fixture.dns.socket = self.transport
        self.verifier = None
        self.prepare_other_wire_evidence()

    def tearDown(self):
        self.transport.release.set()
        if self.handler:
            self.handler.join(5)
            self.assertFalse(self.handler.is_alive())
        if self.verifier:
            self.verifier.join(5)
            self.assertFalse(self.verifier.is_alive())

    def prepare_other_wire_evidence(self):
        for server, host in [(self.fixture.http6, "::1"),
                             (self.fixture.http4, "127.0.0.1")]:
            done = threading.Event()
            original = server.finish_request

            def finish(request, peer, original=original, done=done):
                try:
                    original(request, peer)
                finally:
                    done.set()

            server.finish_request = finish
            connection = http.client.HTTPConnection(host, server.server_port, timeout=5)
            try:
                connection.request("GET", "/ipv6/" + self.fixture.raw.session)
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(response.read(), self.fixture.raw.session.encode())
            finally:
                connection.close()
            self.assertTrue(done.wait(5))
        done = threading.Event()
        original = self.fixture.mqtt.finish_request

        def finish(request, peer):
            try:
                original(request, peer)
            finally:
                done.set()

        self.fixture.mqtt.finish_request = finish
        with socket.create_connection(("::1", self.fixture.mqtt_port), timeout=5) as client:
            session = self.fixture.raw.session.encode()
            connect = b"\x00\x04MQTT\x04\x02\x00\x1e" + struct.pack("!H", len(session)) + session
            client.sendall(b"\x10" + bytes([len(connect)]) + connect)
            response = b""
            while len(response) < 4:
                part = client.recv(4 - len(response))
                self.assertTrue(part)
                response += part
            self.assertEqual(response, b"\x20\x02\x00\x00")
            topic = b"pal/ipv6"
            publish = struct.pack("!H", len(topic)) + topic + session
            client.sendall(b"\x30" + bytes([len(publish)]) + publish + b"\xe0\x00")
        self.assertTrue(done.wait(5))
        self.assertEqual(len(self.fixture.snapshot()["applications"]), 3)

    def query(self, name=None):
        name = name or self.fixture.raw.session + ".ipv6.test"
        labels = b"".join(bytes([len(label)]) + label.encode() for label in name.split("."))
        return struct.pack("!6H", 0x6A31, 0x100, 1, 0, 0, 0) + labels + b"\x00\x00\x1c\x00\x01"

    def test_successful_reply_waits_for_atomic_evidence_publication(self):
        lock = VerifierObservedLock(self.fixture.lock)
        self.fixture.lock = lock
        with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as client:
            client.settimeout(5)
            query = self.query()
            client.sendto(query, ("::1", self.fixture.dns_port))
            response, peer = client.recvfrom(512)
            self.assertEqual(peer[0], "::1")
            self.assertEqual(response[:2], query[:2])
            self.assertEqual(response[12:len(query)], query[12:])
            self.assertEqual(response[-16:], socket.inet_pton(socket.AF_INET6, "::1"))
            self.assertTrue(self.transport.sent.wait(5))
            verified = threading.Event()
            failures = []

            def verify():
                try:
                    self.fixture.verify()
                except Exception as error:
                    failures.append(error)
                finally:
                    verified.set()

            self.verifier = threading.Thread(target=verify)
            lock.verifier = self.verifier
            self.verifier.start()
            self.assertTrue(lock.decided.wait(5))
            if lock.acquired:
                # On the old order this is deterministic: the verifier owns
                # the snapshot lock, and the DNS publisher is still held.
                self.assertTrue(verified.wait(5))
                self.assertEqual(failures, [], "valid wire reply lost its DNS witness")
                self.fail("verify completed before successful reply publication")
            self.assertFalse(verified.is_set())
            self.transport.release.set()
            self.assertTrue(self.done.wait(5))
            self.assertTrue(verified.wait(5))
            self.assertEqual(failures, [])
            self.assertEqual(self.errors, [])
            record = next(r for r in self.fixture.snapshot()["applications"]
                          if r["protocol"] == "dns-aaaa")
            self.assertEqual(record["name"], self.fixture.raw.session + ".ipv6.test")
            self.assertEqual(record["txid"], 0x6A31)
            self.assertEqual(record["answer"], "::1")

    def test_failed_send_does_not_publish_a_success_witness(self):
        self.transport.fail = True
        with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as client:
            client.sendto(self.query(), ("::1", self.fixture.dns_port))
            self.assertTrue(self.done.wait(5))
            client.setblocking(False)
            with self.assertRaises(BlockingIOError):
                client.recvfrom(512)
        self.assertEqual(len(self.errors), 1)
        self.assertEqual(self.errors[0].errno, errno.EIO)
        self.assertFalse(self.transport.sent.is_set())
        with self.assertRaises(AssertionError):
            self.fixture.verify()

    def test_wrong_nonce_does_not_publish_a_dns_witness(self):
        with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as client:
            client.sendto(self.query("wrong.ipv6.test"), ("::1", self.fixture.dns_port))
            self.assertTrue(self.done.wait(5))
            client.setblocking(False)
            with self.assertRaises(BlockingIOError):
                client.recvfrom(512)
        self.assertEqual(self.errors, [])
        self.assertFalse(self.transport.sent.is_set())
        with self.assertRaises(AssertionError):
            self.fixture.verify()


if __name__ == "__main__":
    unittest.main()
