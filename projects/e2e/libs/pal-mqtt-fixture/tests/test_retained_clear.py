"""Real TCP regression for retained clearing, empty echo and clean wire close."""
import socket
import tempfile
import time
import unittest

from mqtt_fixture import Fixture, field, packet, span


def read_packet(connection):
    def exact(length):
        data = bytearray()
        while len(data) < length:
            part = connection.recv(length - len(data))
            if not part:
                raise AssertionError('connection closed before the complete packet')
            data.extend(part)
        return bytes(data)
    kind = exact(1)[0]
    remaining, multiplier = 0, 1
    for _ in range(4):
        digit = exact(1)[0]
        remaining += (digit & 127) * multiplier
        if not digit & 128:
            return kind, exact(remaining)
        multiplier *= 128
    raise AssertionError('invalid MQTT length')


class RetainedClearTest(unittest.TestCase):
    def test_empty_echo_is_consumed_before_wire_disconnect(self):
        with tempfile.TemporaryDirectory() as directory, Fixture(directory) as fixture:
            client_id = fixture.session + '-retained-delivery'
            topic = ('h2/retained/' + fixture.session).encode()
            with socket.create_connection(('127.0.0.1', fixture.tcp.port), timeout=2) as connection:
                connection.sendall(packet(0x10, field(b'MQTT') + b'\x04\x02\x00\x1e' + field(client_id.encode())))
                self.assertEqual(read_packet(connection), (0x20, b'\x00\x00'))
                connection.sendall(packet(0x31, field(topic) + b'original retained payload'))
                connection.sendall(packet(0x82, b'\x00\x01' + field(topic) + b'\x00'))
                self.assertEqual(read_packet(connection), (0x90, b'\x00\x01\x00'))
                kind, body = read_packet(connection)
                self.assertEqual(kind, 0x31)
                returned_topic, offset = span(body, 0)
                self.assertEqual(returned_topic, topic)
                self.assertEqual(body[offset:], b'original retained payload')
                connection.sendall(packet(0x31, field(topic)))
                kind, body = read_packet(connection)
                self.assertEqual(kind, 0x30, 'clearing publish echo must have retain=0 and QoS0')
                returned_topic, offset = span(body, 0)
                self.assertEqual(returned_topic, topic)
                self.assertEqual(body[offset:], b'')
                connection.sendall(packet(0xE0))
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    with fixture.tcp.lock:
                        row = fixture.tcp.arrivals.get(client_id, {}).copy()
                    if row.get('disconnect') == 1:
                        break
                    time.sleep(.01)
                self.assertEqual(row.get('connect'), 1)
                self.assertEqual(row.get('publish'), 2)
                self.assertEqual(row.get('subscribe'), 1)
                self.assertEqual(row.get('disconnect'), 1)
            witness = fixture.verify(full=False)
            self.assertEqual(witness['active_clients'], 0)
            self.assertEqual(witness['retained_messages'], 0)


if __name__ == '__main__':
    unittest.main()
