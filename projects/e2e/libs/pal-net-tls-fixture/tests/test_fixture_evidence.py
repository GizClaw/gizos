import socket
import ssl
import unittest
from fixture import Fixture, packet, payload


class Evidence(unittest.TestCase):
    def test_control_rejects_wrong_session(self):
        with Fixture() as fixture:
            with socket.create_connection((fixture.bind, fixture.port)) as connection:
                connection.sendall(b'ARM missing tls-required 2 0\n')
                self.assertEqual(connection.recv(100), b'FAIL 0\n')
            self.assertFalse(fixture.records)

    def test_real_fixture_echo_and_certificate_evidence(self):
        with Fixture() as fixture:
            case = 'tls-sni-alpn'
            port = fixture.arm(case, 2, 0, fixture.bind)
            context = ssl.create_default_context(cafile=str(fixture.ca))
            context.set_alpn_protocols(['h2-pal-e2e'])
            with socket.create_connection((fixture.bind, port)) as raw:
                with context.wrap_socket(raw, server_hostname='pal-net-tls.test') as connection:
                    connection.sendall(packet(fixture.session, case))
                    received = bytearray()
                    while len(received) < len(payload()):
                        part = connection.recv(257)
                        self.assertTrue(part)
                        received.extend(part)
                    self.assertEqual(bytes(received), bytes(value ^ 0xa5 for value in payload()))
            self.assertTrue(fixture.proof(case, 2, timeout=2)['handshake_succeeded'])
            for case, mode, ca, host in (
                ('tls-wrong-ca', 2, fixture.wrong_ca, 'pal-net-tls.test'),
                ('tls-wrong-name', 2, fixture.ca, 'wrong-pal-net-tls.test'),
                ('tls-expired', 3, fixture.ca, 'pal-net-tls.test'),
            ):
                port = fixture.arm(case, mode, 0, fixture.bind)
                context = ssl.create_default_context(cafile=str(ca))
                with socket.create_connection((fixture.bind, port)) as raw:
                    with self.assertRaises(ssl.SSLCertVerificationError):
                        context.wrap_socket(raw, server_hostname=host)
                proof = fixture.proof(case, 1, timeout=2)
                self.assertTrue(proof['client_hello'])
                self.assertTrue(proof['certificate_presented'])
                self.assertFalse(proof['handshake_succeeded'])
                self.assertTrue(proof['peer_alerts'])

    def test_rejection_cannot_be_satisfied_by_connection_failure(self):
        with Fixture() as fixture:
            port = fixture.arm('tls-wrong-ca', 2, 0, fixture.bind)
            with socket.create_connection((fixture.bind, port)):
                pass
            with self.assertRaises(RuntimeError):
                fixture.proof('tls-wrong-ca', 1, timeout=.1)
            record = fixture.records['tls-wrong-ca']
            record['client_hello'] = True
            record['certificate_presented'] = True
            record['finished'] = True
            record['handshake_succeeded'] = True
            with self.assertRaises(RuntimeError):
                fixture.proof('tls-wrong-ca', 1)
            record['handshake_succeeded'] = False
            record['payload_received'] = 1
            with self.assertRaises(RuntimeError):
                fixture.proof('tls-wrong-ca', 1)
            record['payload_received'] = 0
            record['certificate_presented'] = False
            with self.assertRaises(RuntimeError):
                fixture.proof('tls-wrong-ca', 1)

    def test_corrupt_payload_rejected(self):
        with Fixture() as fixture:
            case = 'tcp-echo'
            port = fixture.arm(case, 0, 0, fixture.bind)
            request = bytearray(packet(fixture.session, case))
            request[-1] ^= 1
            with socket.create_connection((fixture.bind, port)) as connection:
                connection.sendall(request)
            with self.assertRaises(RuntimeError):
                fixture.proof(case, 0, timeout=.1)


if __name__ == '__main__':
    unittest.main()
