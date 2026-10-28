import socket
import sys
import threading
import unittest
from fixture import (Fixture, MULTICAST_GROUP, MULTICAST_DST_OPTION,
                     multicast_packet, multicast_destination)


class DatagramPeer:
    def __init__(self, session, fault=None):
        self.session = session
        self.fault = fault
        self.index = 0
        self.sent = []
        self.closed = False

    def recvmsg(self, size, ancillary_size):
        data = multicast_packet(self.session, 'multicast-data', self.index)
        destination = '192.0.2.2' if self.fault == 'unicast' else MULTICAST_GROUP
        address = socket.inet_aton(destination)
        control = address if sys.platform == 'darwin' else bytes(8) + address
        ancillary = [(socket.IPPROTO_IP, MULTICAST_DST_OPTION, control)]
        if self.fault == 'missing-destination':
            ancillary = []
        if self.fault == 'corrupt':
            data = data[:-1] + bytes([data[-1] ^ 1])
        if self.fault == 'stale-sequence':
            data = multicast_packet(self.session, 'multicast-data', 2)
        peer = ('192.0.2.9' if self.fault == 'wrong-peer' else '192.0.2.1', 42000)
        if self.fault == 'timeout':
            raise socket.timeout('controlled timeout')
        self.index += 1
        return data, ancillary, 0, peer

    def sendto(self, data, destination):
        self.sent.append((data, destination))
        return len(data)

    def close(self):
        self.closed = True


class MulticastEvidence(unittest.TestCase):
    @unittest.skipUnless(sys.platform in ("darwin", "linux"),
                         "requires POSIX receive-destination ancillary data")
    def test_local_multicast_socket_exchange(self):
        with Fixture(local_multicast=True) as fixture:
            self.assertEqual(fixture.bind, "127.0.0.1")
            interface = fixture.multicast_bind
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                peer.bind(("0.0.0.0", 0))
                peer.settimeout(3)
                peer.setsockopt(socket.IPPROTO_IP, MULTICAST_DST_OPTION, 1)
                peer.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                                socket.inet_aton(MULTICAST_GROUP) + socket.inet_aton(interface))
                peer.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                                socket.inet_aton(interface))
                peer.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
                peer.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
                callback_port = peer.getsockname()[1]
                port = fixture.arm('multicast-data', 7, callback_port, '127.0.0.1')
                for sequence in range(3):
                    request = multicast_packet(fixture.session, 'multicast-data', sequence)
                    self.assertEqual(peer.sendto(request, (MULTICAST_GROUP, port)), 224)
                    response, ancillary, flags, source = peer.recvmsg(4096, 128)
                    self.assertEqual(flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC), 0)
                    self.assertEqual(multicast_destination(ancillary), MULTICAST_GROUP)
                    self.assertEqual(source, (interface, port))
                    self.assertEqual(response, bytes(value ^ 0xa5 for value in request))
                proof = fixture.proof('multicast-data', 6, timeout=2)
                self.assertEqual(proof['packets_received'], 3)
                self.assertEqual(proof['packets_sent'], 3)

    def test_multicast_proof_requires_destination_identity_and_all_packets(self):
        for fault in (None, 'unicast', 'missing-destination', 'wrong-peer',
                      'corrupt', 'stale-sequence', 'timeout'):
            with self.subTest(fault=fault):
                fixture = Fixture.__new__(Fixture)
                fixture.session = '0123456789abcdef0123456789abcdef'
                fixture.lock = threading.Lock()
                record = dict(session=fixture.session, run_id=fixture.session[:16],
                              case='multicast-data', mode=7, peer='192.0.2.1',
                              callback_port=42000, accepted=False, finished=False,
                              payload_valid=False, payload_received=0, payload_sent=0,
                              packets_received=0, packets_sent=0,
                              multicast_destinations=[], multicast_sent_destinations=[],
                              error=None)
                fixture.records = {'multicast-data': record}
                peer = DatagramPeer(fixture.session, fault)
                fixture.multicast(peer, record)
                self.assertTrue(peer.closed)
                if fault:
                    with self.assertRaises(RuntimeError):
                        fixture.proof('multicast-data', 6)
                    self.assertFalse(peer.sent)
                else:
                    self.assertEqual(fixture.proof('multicast-data', 6)['payload_received'], 672)
                    self.assertEqual(record['payload_sent'], 672)
                    for sequence, (data, target) in enumerate(peer.sent):
                        self.assertEqual(target, (MULTICAST_GROUP, 42000))
                        self.assertEqual(data, bytes(x ^ 0xa5 for x in
                            multicast_packet(fixture.session, 'multicast-data', sequence)))
                    record['packets_received'] = 2
                    with self.assertRaises(RuntimeError):
                        fixture.proof('multicast-data', 6)


if __name__ == '__main__':
    unittest.main()
