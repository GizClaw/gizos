"""Independently inventory public Audio PAL operations against the E2E ledger.

This static check prevents a new public operation from silently missing cases.
It does not replace real platform execution or compiler coverage.
"""

import json
from pathlib import Path
import re
import unittest

ROOT = Path.cwd()
APP = ROOT / "projects/e2e/apps/pal-audio/app"
HEADER = ROOT / "libs/pal/include/h2/pal/hal/h2_pal_audio.h"


class InterfaceCoverage(unittest.TestCase):
    def test_all_public_operations_have_executed_case_sites(self):
        header = HEADER.read_text()
        manifest = json.loads((APP / "api_coverage.json").read_text())
        registry = (APP / "include/h2_pal_audio_cases.inc").read_text()
        source = (APP / "src/h2_pal_audio_e2e.c").read_text()
        provider = re.search(r"typedef struct h2_pal_audio_vtable\s*\{(.*?)\}\s*h2_pal_audio_vtable_t;",
                             header, re.S).group(1)
        provider_ops = set(re.findall(r"\(\*(\w+)\)\s*\(", provider))
        track = re.search(r"struct h2_pal_audio_track\s*\{(.*?)\};", header, re.S).group(1)
        track_ops = set(re.findall(r"h2_pal_audio_track_\w+_fn\s+(\w+)\s*;", track))
        self.assertEqual(provider_ops, set(manifest["provider_operations"]))
        self.assertEqual(track_ops, set(manifest["track_operations"]))
        self.assertEqual(len(provider_ops), 12)
        self.assertEqual(len(track_ops), 5)
        cases = re.findall(r'H2_PAL_AUDIO_CASE\(\w+, "([^"]+)"\)', registry)
        self.assertEqual(len(cases), len(set(cases)))
        self.assertEqual(len(cases), 24)
        for operation, ids in manifest["provider_operations"].items():
            self.assertTrue(ids, operation)
            self.assertIn(f"h2_pal_audio_{operation}(", source)
            self.assertTrue(set(ids) <= set(cases), operation)
        for operation, ids in manifest["track_operations"].items():
            self.assertTrue(ids, operation)
            self.assertIn(f"h2_pal_audio_track_{operation}(", source)
            self.assertTrue(set(ids) <= set(cases), operation)
        self.assertEqual(manifest["separate_capability"], "h2_pal_audio_decoder.h")


if __name__ == "__main__":
    unittest.main()
