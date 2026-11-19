"""Check the installed Zero BK fixed-XIP ABI in both generated SDK tables."""

import csv
from pathlib import Path
import sys
import unittest


def parse_size(value):
    if value.lower().endswith("k"):
        return int(value[:-1]) * 1024
    return int(value, 0)


def read_table(path):
    table = {}
    end = 0
    with Path(path).open() as file:
        for name, offset, size, kind, readable, writable in csv.reader(
            line for line in file if line.strip() and not line.startswith("#")
        ):
            start = parse_size(offset) if offset else end
            end = start + parse_size(size)
            if name in table:
                raise ValueError(f"duplicate partition {name}")
            table[name] = (start, end, kind, readable, writable)
    return table


class FixedXipTest(unittest.TestCase):
    def test_executing_and_update_windows(self):
        p1, p2 = map(read_table, sys.argv[1:3])
        for table, cp, ap, end, other_start, other_end in [
            (p1, 0x11000, 0x132000, 0x275000, 0x275000, 0x75F000),
            (p2, 0x275000, 0x396000, 0x75F000, 0x11000, 0x275000),
        ]:
            self.assertEqual(table.pop("primary_cp_app"), (cp, ap, "code", "TRUE", "TRUE"))
            self.assertEqual(table.pop("primary_ap_app"), (ap, end, "code", "TRUE", "TRUE"))
            self.assertEqual(table.pop("s_app"), (other_start, other_end, "data", "TRUE", "TRUE"))
        self.assertEqual(p1, p2)
        self.assertEqual(p1.pop("primary_bootloader"), (0, 0x11000, "code", "TRUE", "FALSE"))
        tail = [
            ("ota_fina_executive", 0x75F000), ("usr_config", 0x760000),
            ("h2_boot_request", 0x77F000), ("flashdb", 0x780000),
            ("coredump", 0x7A0000), ("easyflash", 0x7FA000),
            ("easyflash_ap", 0x7FC000), ("sys_rf", 0x7FE000), ("sys_net", 0x7FF000),
        ]
        ends = [offset for _, offset in tail[1:]] + [0x800000]
        self.assertEqual(p1, {name: (start, end, "data", "TRUE", "TRUE")
                              for (name, start), end in zip(tail, ends)})


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
