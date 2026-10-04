import socket
import ssl
import tempfile
import time
import unittest

from mqtt_fixture import Fixture


def completed_handshake(fixture):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        rows = fixture.tls.handshake_snapshot()
        if rows and rows[-1]['finished']:
            return rows[-1]
        time.sleep(0.01)
    raise AssertionError('real server handshake did not finish')


class TLSDiagnosticsTest(unittest.TestCase):
    def test_silent_peer_records_actual_server_timeout_and_cannot_prove_certificate_rejection(self):
        with tempfile.TemporaryDirectory() as directory, Fixture(directory, tls_handshake_timeout=0.15) as fixture:
            with socket.create_connection(('127.0.0.1', fixture.tls.port)):
                row = completed_handshake(fixture)
            self.assertFalse(row['succeeded'])
            self.assertFalse(row['client_hello'])
            self.assertFalse(row['certificate_presented'])
            self.assertEqual(row['timeout_seconds'], 0.15)
            self.assertEqual(row['error']['type'], 'TimeoutError')
            self.assertGreaterEqual(row['elapsed_ms'], 100)
            self.assertEqual(row['peer_ip'], '127.0.0.1')
            self.assertGreater(row['peer_port'], 0)
            self.assertNotIn('_started_monotonic', row)
            with self.assertRaisesRegex(RuntimeError, 'TLS rejection lacks two real certificate handshakes'):
                fixture.verify()

    def test_trusted_handshake_preserves_default_budget_and_records_real_tls_messages(self):
        with tempfile.TemporaryDirectory() as directory, Fixture(directory) as fixture:
            context = ssl.create_default_context(cafile=str(fixture.ca))
            with socket.create_connection(('127.0.0.1', fixture.tls.port)) as raw:
                with context.wrap_socket(raw, server_hostname='127.0.0.1'):
                    row = completed_handshake(fixture)
            self.assertTrue(row['succeeded'])
            self.assertTrue(row['client_hello'])
            self.assertTrue(row['certificate_presented'])
            self.assertEqual(row['timeout_seconds'], 2)
            self.assertTrue(row['tls_version'].startswith('TLS'))
            self.assertTrue(row['cipher'][0])
            self.assertNotIn('error', row)
            self.assertTrue(any(message['direction'] == 'read' and message['message_type'] == 1
                                for message in row['messages']))
            self.assertTrue(any(message['direction'] == 'write' and message['message_type'] == 11
                                for message in row['messages']))
            headers = [message for message in row['messages'] if message['content_type'] == 256]
            self.assertTrue(headers)
            self.assertTrue(all(message['data_length'] == 5 and message['record_length'] >= 0
                                for message in headers))
            self.assertEqual(fixture.tls.diagnostic_snapshot()['failures'], [])


if __name__ == '__main__':
    unittest.main()
