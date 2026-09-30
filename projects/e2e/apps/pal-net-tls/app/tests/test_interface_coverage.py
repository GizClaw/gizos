import json
from pathlib import Path
import re
import unittest


class Interface(unittest.TestCase):
    def test_all_public_slots_mapped(self):
        root = Path(__file__).resolve().parents[6]
        app = root / 'projects/e2e/apps/pal-net-tls/app'
        header = (root / 'libs/pal/include/h2/pal/net/h2_pal_net.h').read_text()
        inventory = json.loads((app / 'api_coverage.json').read_text())
        slots = re.findall(r'\(\*(\w+)\)', header)
        self.assertEqual(len(slots), 21)
        self.assertEqual(set(slots), set(inventory['slots']))
        registry = dict((case, int(required)) for case, required in re.findall(
            r'H2_NET_TLS_CASE\([^,]+, "([^"]+)", ([01])\)',
            (app / 'include/h2_pal_net_tls_cases.inc').read_text()))
        self.assertEqual(len(registry), 39)
        self.assertEqual(sum(registry.values()), 37)
        source = (app / 'src/h2_pal_net_tls_e2e.c').read_text()
        for slot, cases in inventory['slots'].items():
            self.assertTrue(cases, slot)
            self.assertTrue(set(cases).issubset(registry), slot)
            self.assertTrue('h2_pal_net_' + slot + '(' in source or '->' + slot + '(' in source, slot)
        self.assertFalse(inventory['full_net_qualification'])
        self.assertEqual(set(inventory['optional_capabilities']), {'icmp_echo', 'udp_join_multicast'})


if __name__ == '__main__':
    unittest.main()
