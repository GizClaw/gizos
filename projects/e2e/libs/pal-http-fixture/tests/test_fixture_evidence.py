"""TLS proof must reflect a real presented-and-rejected server certificate."""
import http.client
import socket
import ssl
import tempfile
import time
import unittest
from urllib.parse import urlparse
from fixture import Fixture, TLSRejectionLedger


class FixtureEvidence(unittest.TestCase):
    def test_absence_disconnect_and_success_do_not_prove_certificate_rejection(self):
        ledger = TLSRejectionLedger()
        with self.assertRaises(RuntimeError):
            ledger.proof('run')
        ledger.arm('peer', 'run')
        with self.assertRaises(RuntimeError):
            ledger.proof('run')
        event = ledger.accepted('peer')
        ledger.finished(event, False, 'UNEXPECTED_EOF')
        with self.assertRaises(RuntimeError):
            ledger.proof('run')
        ledger.arm('peer', 'run')
        event = ledger.accepted('peer')
        ledger.message(event, 'read', 22, 1, b'hello')
        ledger.message(event, 'write', 22, 11, b'certificate')
        ledger.finished(event, True)
        ledger.finished(event, False, 'late disconnect')
        with self.assertRaises(RuntimeError):
            ledger.proof('run')

    def test_real_untrusted_handshake_proof_is_not_reusable(self):
        with tempfile.TemporaryDirectory() as directory, Fixture(directory) as fixture:
            endpoint = urlparse(fixture.http)
            client = http.client.HTTPConnection(endpoint.hostname, endpoint.port, timeout=5)
            client.request('GET', endpoint.path + '/tls-rejection/arm')
            response = client.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), b'armed')
            client.close()
            bad = urlparse(fixture.untrusted)
            trust = ssl.create_default_context(cadata=fixture.ca.read_text())
            with socket.create_connection((bad.hostname, bad.port), timeout=5) as raw:
                with self.assertRaises(ssl.SSLCertVerificationError):
                    trust.wrap_socket(raw, server_hostname=bad.hostname)
            proof = None
            for _ in range(40):
                try:
                    proof = fixture.verify_tls_rejection()
                    break
                except RuntimeError:
                    time.sleep(0.025)
            self.assertIsNotNone(proof)
            self.assertTrue(proof['verified'])
            self.assertEqual(proof['http_requests'], 0)
            self.assertTrue(any(event['certificate_presented'] for event in proof['events']))
            fixture.rejections.arm('127.0.0.1', '')
            with self.assertRaises(RuntimeError):
                fixture.verify_tls_rejection()


if __name__ == '__main__':
    unittest.main()
