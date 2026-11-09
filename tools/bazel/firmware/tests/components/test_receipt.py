"""Check that the SDK source ABI boundary retains a complete input receipt."""

import json
from pathlib import Path
import sys
import unittest


RECEIPTS = [json.loads(Path(value).read_text()) for value in sys.argv[1:]]


class FirmwareComponentReceiptTest(unittest.TestCase):
    def test_mixed_and_source_only_components(self):
        self.assertEqual(len(RECEIPTS), 2)
        for receipt, archive_count in zip(RECEIPTS, (2, 0)):
            with self.subTest(archive_count=archive_count):
                components = receipt["components"]
                self.assertEqual(sum(component["archive"] is not None for component in components), archive_count)
                generated = [component for component in components if component["generate_component"]]
                self.assertEqual(len(generated), 1)
                component = generated[0]
                self.assertEqual(component["component_name"], "h2_firmware_lib")
                self.assertEqual(len(component["native_srcs"]), 1)
                source = component["native_srcs"][0]
                self.assertTrue(source.endswith("/portable.c"))
                self.assertIn(source, receipt["inputs"])
                for suffix in ("/include/portable.h", "/explicit/explicit.h"):
                    headers = [header for header in component["headers"] if header.endswith(suffix)]
                    self.assertEqual(len(headers), 1)
                    self.assertIn(headers[0], receipt["inputs"])
                self.assertTrue(any(root.endswith("/include") for root in component["include_roots"]))
                self.assertTrue(any(root.endswith("/explicit") for root in component["include_roots"]))
                self.assertEqual(receipt["tasks"], ["fixture/serial", "fixture/startup"])
                self.assertEqual(len({entry["component_name"] for entry in components}), len(components))
                for entry in components[1:]:
                    self.assertEqual(entry["native_srcs"], [])
                    self.assertEqual(entry["headers"], [])
                    self.assertFalse(entry["generate_component"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
