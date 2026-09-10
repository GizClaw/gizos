"""Each BK7258 board's Loader and App partition tables describe one flash.

The fixed-XIP code derives both windows from whichever table the running
image carries, so the two tables of a board must agree: the Loader table's
s_app is the App window, the App table's s_app is the Loader window, and every
shared partition sits at the same place.
"""

import os
import unittest
from pathlib import Path

WINDOW_ALIGN = 68 * 1024
# (board, Loader layout table, App layout table), relative to the workspace.
BOARDS = [
    (
        "bk7258_v3_202405",
        "boards/bk7258_v3_202405/bk7258/layouts/loader/partitions/bk7258/auto_partitions.csv",
        "boards/bk7258_v3_202405/bk7258/layouts/h2loader/partitions/bk7258/auto_partitions.csv",
    ),
]


def parse_size(text: str) -> int:
    text = text.strip()
    for suffix, scale in (("k", 1024), ("K", 1024), ("m", 1 << 20), ("M", 1 << 20)):
        if text.endswith(suffix):
            return int(text[:-1], 0) * scale
    return int(text, 0)


def parse_table(path: Path) -> dict[str, tuple[int, int]]:
    """Name -> (offset, size); an empty offset follows the previous row."""
    rows: dict[str, tuple[int, int]] = {}
    cursor = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        name, offset, size = (field.strip() for field in line.split(",")[:3])
        start = int(offset, 0) if offset else cursor
        length = parse_size(size)
        rows[name] = (start, length)
        cursor = start + length
    return rows


def own_window(table: dict[str, tuple[int, int]]) -> tuple[int, int]:
    cp_offset, cp_size = table["primary_cp_app"]
    ap_offset, ap_size = table["primary_ap_app"]
    assert cp_offset + cp_size == ap_offset, "CP and AP must be contiguous"
    return cp_offset, cp_size + ap_size


class BoardLayoutTest(unittest.TestCase):
    def test_loader_and_app_tables_agree(self):
        root = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        for board, loader_path, app_path in BOARDS:
            with self.subTest(board=board):
                loader = parse_table(root / loader_path)
                app = parse_table(root / app_path)
                loader_window = own_window(loader)
                app_window = own_window(app)
                self.assertEqual(loader["s_app"], app_window)
                self.assertEqual(app["s_app"], loader_window)
                for offset, size in (loader_window, app_window):
                    self.assertEqual(offset % WINDOW_ALIGN, 0)
                    self.assertEqual(size % WINDOW_ALIGN, 0)
                # Loader self-update stages the Loader image in the App window.
                self.assertGreaterEqual(app_window[1], loader_window[1])
                self.assertIn("h2_boot_request", loader)
                shared = set(loader) - {"primary_cp_app", "primary_ap_app", "s_app"}
                self.assertEqual(shared, set(app) - {"primary_cp_app", "primary_ap_app", "s_app"})
                for name in shared:
                    self.assertEqual(loader[name], app[name], name)


if __name__ == "__main__":
    unittest.main()
