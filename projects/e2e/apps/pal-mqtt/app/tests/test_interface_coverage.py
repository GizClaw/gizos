"""Match every MQTT typed operation and case against the production header."""
import json
from pathlib import Path
import re
import unittest


class Inventory(unittest.TestCase):
    def test_public_contract(self):
        root = Path(__file__).absolute().parents[6]
        app = root / 'projects/e2e/apps/pal-mqtt/app'
        inventory = json.loads((app / 'api_coverage.json').read_text())
        header = (root / inventory['header']).read_text()
        vtable = re.search(r'typedef struct h2_pal_mqtt_vtable\s*\{(.*?)\}', header, re.S).group(1)
        self.assertEqual(set(re.findall(r'\(\*(\w+)\)', vtable)), set(inventory['operations']))
        registry = (app / 'include/h2_pal_mqtt_cases.inc').read_text()
        cases = re.findall(r'H2_PAL_MQTT_CASE\(\w+, "([^"]+)"\)', registry)
        self.assertEqual(cases, inventory['cases'])
        self.assertEqual(len(cases), len(set(cases)))
        source = (app / 'src/h2_pal_mqtt_e2e.c').read_text()
        for name, entry in inventory['operations'].items():
            self.assertIn(entry['wrapper'] + '(', header)
            self.assertIn(entry['wrapper'] + '(', source)
            self.assertTrue(entry['cases'])
            self.assertTrue(set(entry['cases']).issubset(cases), name)
        for case in cases:
            self.assertIn('H2_PAL_MQTT_E2E_' + case.upper().replace('-', '_'), source)


if __name__ == '__main__':
    unittest.main()
