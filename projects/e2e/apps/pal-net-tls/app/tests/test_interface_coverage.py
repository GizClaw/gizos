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
        vtable = header.split('typedef struct h2_pal_net_vtable {', 1)[1].split('} h2_pal_net_vtable_t;', 1)[0]
        slots = re.findall(r'\(\*(\w+)\)', vtable)
        self.assertEqual(len(slots), 25)
        self.assertEqual(set(slots), set(inventory['slots']) |
            {'resolve_all', 'resolve_start_family', 'resolve_poll_all', 'get_host_addr_family'})
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
        ipv6 = root / 'projects/e2e/apps/pal-ipv6/app'
        ipv6_ids = set(re.findall(r'H2_PAL_IPV6_CASE\(\w+, "([^\"]+)"\)',
            (ipv6 / 'include/h2_pal_ipv6_cases.inc').read_text()))
        ipv6_source = (ipv6 / 'src/h2_pal_ipv6_e2e.c').read_text()
        extensions = {
            'resolve_all': ['dns-ipv4-filter', 'dns-ipv6-filter', 'dns-any-families'],
            'resolve_start_family': ['dns-family-async-copy', 'dns-family-cancel'],
            'resolve_poll_all': ['dns-family-async-copy'],
            'get_host_addr_family': ['netif-ipv6-address'],
        }
        for slot, cases in extensions.items():
            self.assertTrue(set(cases).issubset(ipv6_ids), slot)
            self.assertIn('h2_pal_net_' + slot + '(', ipv6_source)
        self.assertFalse(inventory['full_net_qualification'])
        self.assertEqual(set(inventory['optional_capabilities']), {'icmp_echo', 'udp_join_multicast'})


if __name__ == '__main__':
    unittest.main()
