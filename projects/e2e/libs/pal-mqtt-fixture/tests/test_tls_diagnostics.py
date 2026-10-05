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

    def test_real_rejections_record_distinct_case_names_and_tls_alerts(self):
        with tempfile.TemporaryDirectory() as directory, Fixture(directory) as fixture:
            fixture.tls.current_run_by_peer['127.0.0.1']=fixture.session
            for ca,name,reason in [(fixture.wrong_ca,'localhost','TLSV1_ALERT_UNKNOWN_CA'),
                                   (fixture.ca,'wrong-name.invalid','SSLV3_ALERT_BAD_CERTIFICATE')]:
                context=ssl.create_default_context(cafile=str(ca))
                previous=len(fixture.tls.handshake_snapshot())
                with socket.create_connection(('127.0.0.1',fixture.tls.port),timeout=3) as raw:
                    # Automatic wrap_socket handshakes close on verification
                    # failure, which can reset the stream before Windows has
                    # delivered the fatal alert. Keep the real SSL socket open
                    # until the server has observed the completed handshake.
                    with context.wrap_socket(raw,server_hostname=name,do_handshake_on_connect=False) as connection:
                        with self.assertRaises(ssl.SSLCertVerificationError):connection.do_handshake()
                        deadline=time.monotonic()+3
                        while time.monotonic()<deadline:
                            rows=fixture.tls.handshake_snapshot()
                            if len(rows)>previous and rows[-1]['finished']:break
                            time.sleep(0.01)
                self.assertGreater(len(rows),previous)
                self.assertTrue(rows[-1]['finished'])
                self.assertEqual(rows[-1]['server_name'],name)
                self.assertEqual(rows[-1]['error']['reason'],reason,rows[-1]['error'])
                self.assertTrue(rows[-1]['client_hello'] and rows[-1]['certificate_presented'])
                self.assertFalse(rows[-1]['succeeded'])
            # Missing observed SNI must not be accepted as the normal-name
            # rejection, even when the real fatal alert and Certificate exist.
            for missing in [None, '']:
                fixture.tls.handshakes[0]['server_name']=missing
                with self.assertRaisesRegex(RuntimeError,'server-name'):
                    fixture.verify()
            del fixture.tls.handshakes[0]['server_name']
            with self.assertRaisesRegex(RuntimeError,'server-name'):
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
