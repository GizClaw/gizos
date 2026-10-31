"""Reject a diagnostic layout that writes outside its own reduced App slot."""

import argparse
from pathlib import Path
import unittest


def size(value):
    value = value.strip().lower()
    return int(value[:-1], 0) * 1024 if value.endswith("k") else int(value, 0)


def partitions(path):
    result = {}
    end = 0
    for line in Path(path).read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        name, offset, length, kind, readable, writable = (
            item.strip() for item in line.split(",")
        )
        start = size(offset) if offset else end
        end = start + size(length)
        if name in result:
            raise ValueError(f"duplicate partition: {name}")
        result[name] = (start, end, kind, readable, writable)
    return result


class LayoutTest(unittest.TestCase):
    def test_only_app_tail_is_reassigned(self):
        self.assertEqual(set(diagnostic), set(baseline) | {"pal_disk"})
        for name in baseline:
            if name != "primary_ap_app":
                self.assertEqual(diagnostic[name], baseline[name], name)
        old = baseline["primary_ap_app"]
        app = diagnostic["primary_ap_app"]
        scratch = diagnostic["pal_disk"]
        self.assertEqual(app[0], old[0])
        self.assertEqual(app[1], scratch[0])
        self.assertEqual(scratch[1], old[1])
        self.assertEqual(scratch[1], diagnostic["ota_fina_executive"][0])
        self.assertEqual(scratch[2:], ("data", "TRUE", "TRUE"))

    def test_native_code_and_erase_alignment(self):
        app = diagnostic["primary_ap_app"]
        scratch = diagnostic["pal_disk"]
        self.assertEqual((app[1] - app[0]) % (34 * 1024), 0)
        self.assertEqual(scratch[0] % 4096, 0)
        self.assertEqual((scratch[1] - scratch[0]) % 4096, 0)
        self.assertGreaterEqual(scratch[1] - scratch[0], 2 * 4096)
        self.assertGreater(app[1], app[0])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--diagnostic", required=True)
    args, rest = parser.parse_known_args()
    baseline = partitions(args.baseline)
    diagnostic = partitions(args.diagnostic)
    unittest.main(argv=[__file__, *rest])
