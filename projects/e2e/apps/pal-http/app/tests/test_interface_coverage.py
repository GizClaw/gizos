"""Independently inventory production HTTP operations and request fields."""
import json
from pathlib import Path
import re
import unittest


class Inventory(unittest.TestCase):
    def test_public_contract(self):
        root = Path(__file__).absolute().parents[6]
        app = root / 'projects/e2e/apps/pal-http/app'
        inventory = json.loads((app / 'api_coverage.json').read_text())
        header = (root / inventory['header']).read_text()
        vtable = re.search(r'typedef struct h2_pal_http_vtable\s*\{(.*?)\}', header, re.S).group(1)
        operations = re.findall(r'\(\*(\w+)\)', vtable)
        self.assertEqual(set(operations), set(inventory['operations']))
        request = re.search(r'typedef struct h2_pal_http_request\s*\{(.*?)\}', header, re.S).group(1)
        request = re.sub(r'/\*.*?\*/', '', request, flags=re.S)
        fields = re.findall(r'(\w+)\s*;', request)
        self.assertEqual(fields, inventory['request_fields'])
        registry = (app / 'include/h2_pal_http_cases.inc').read_text()
        cases = re.findall(r'H2_PAL_HTTP_CASE\(\w+, "([^"]+)"\)', registry)
        self.assertEqual(cases, inventory['cases'])
        self.assertEqual(len(cases), len(set(cases)))
        source = (app / 'src/h2_pal_http_e2e.c').read_text()
        for entry in inventory['operations'].values():
            self.assertIn(entry['wrapper'] + '(', source)
            self.assertTrue(set(entry['cases']).issubset(cases))
        for case in cases:
            self.assertIn('H2_PAL_HTTP_E2E_' + case.upper().replace('-', '_'), source)


if __name__ == '__main__':
    unittest.main()
