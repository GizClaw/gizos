"""Keep committed AC791N evidence reviewable without raw captures or local drafts."""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
EVIDENCE = ROOT / 'guides/apps/h2loader/boards/jieli_ac791n_devkit/evidence/2026-09-14'


class EvidenceTest(unittest.TestCase):
    def test_owned_inputs_only(self):
        self.assertEqual(sorted(p.name for p in EVIDENCE.iterdir()
                                if p.suffix in ('.log', '.status', '.orig', '.rej')), [])
        for path in EVIDENCE.iterdir():
            if path.suffix not in ('.md', '.json'):
                continue
            with self.subTest(path=path.name):
                # Local historical filenames may be retained as provenance, but
                # Markdown links must never depend on a discarded raw capture.
                targets = re.findall(r'\]\(([^)]+)\)', path.read_text())
                self.assertFalse(any(re.search(r'\.(?:log|status)(?:#.*)?$', t)
                                     for t in targets))

    def test_independent_status_facts_are_inline(self):
        uart = (EVIDENCE / 'loader-uart-lifecycle.md').read_text()
        ble = (EVIDENCE / 'loader-ble-lifecycle.md').read_text()
        snapshots = json.loads((EVIDENCE / 'loader-artifacts.json').read_text())['independent_uart_status']
        self.assertEqual(len(snapshots), 7)
        for name, status in snapshots.items():
            doc = ble if name.startswith('ble_') else uart
            with self.subTest(snapshot=name):
                for field in ('running_partition', 'stage_valid'):
                    self.assertIn(f'{field}={status[field]}', doc)
                for field in ('partition_1_image_checksum', 'partition_2_image_checksum'):
                    self.assertIn(status[field], doc)
        self.assertIn('stage_valid=0', uart)
        self.assertIn('25/25', uart)
        self.assertIn('22/22', ble)


if __name__ == '__main__':
    unittest.main()
