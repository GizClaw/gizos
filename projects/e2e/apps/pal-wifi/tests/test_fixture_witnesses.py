"""Reject missing, replayed or wrong-mode physical client observations."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path("projects/e2e/apps/pal-wifi").resolve()))
from fixture_validator import verify_peer_witnesses

MAC = "30eda0ae0f84"
IP = 3232236546


def case(ms, name, elapsed):
    return f'I ({ms}) pal-wifi: H2_WIFI_CASE ' + json.dumps(
        dict(id=name, status="PASS", elapsed_ms=elapsed))


def address(ms):
    return f'I ({ms}) pal-wifi: H2_WIFI_ADDRESS lease={IP} ap=3232236545 mask=4294967040'


def client(ms, joined, ip=IP):
    return (f'I ({ms}) pal-wifi: H2_WIFI_FIXTURE_CLIENT target=h2wifi-dut-esp '
            f'joined={joined} ip4={ip} mac={MAC}')


class Witnesses(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dut = Path(self.tmp.name) / 'dut.log'
        self.fixture = Path(self.tmp.name) / 'fixture.log'
        self.dut_lines = [address(10000), case(10500, 'access-point-real-client', 1000),
                          case(15000, 'access-point-client-left', 4500),
                          address(20000), case(25000, 'access-point-open', 6000),
                          address(30000), case(35000, 'access-point-hidden', 6000)]
        self.fixture_lines = [client(ms, i) for i, ms in enumerate(
            (1000, 5000, 11000, 15000, 21000, 25000), 1)]

    def verify(self, repeats=2):
        self.dut.write_text('\n'.join(self.dut_lines) + '\n')
        self.fixture.write_text('\n'.join(self.fixture_lines) + '\n')
        return verify_peer_witnesses(self.dut, self.fixture, 'h2wifi-dut-esp', MAC, repeats)

    def test_two_fresh_clients_per_mode(self):
        self.assertEqual(len(self.verify()), 6)

    def test_missing_second_client_rejected(self):
        self.fixture_lines.pop()
        with self.assertRaises(AssertionError): self.verify()

    def test_wrong_mode_time_rejected(self):
        self.fixture_lines[3] = client(27000, 4)
        with self.assertRaises(AssertionError): self.verify()

    def test_wrong_peer_ip_rejected(self):
        self.fixture_lines[4] = client(21000, 5, IP + 1)
        with self.assertRaises(AssertionError): self.verify()

    def test_invalid_dut_subnet_rejected(self):
        self.dut_lines[0] = self.dut_lines[0].replace('ap=3232236545', 'ap=3232240001')
        with self.assertRaises(AssertionError): self.verify()

    def test_source_bound_legacy_seed_has_one_per_mode(self):
        self.fixture_lines = [client(ms, i) for i, ms in enumerate((1000, 11000, 21000), 1)]
        self.assertEqual(len(self.verify(repeats=1)), 3)


if __name__ == '__main__': unittest.main()
