import tempfile
import unittest
from pathlib import Path

from list_apps import discover_apps


class InventoryTest(unittest.TestCase):
    def test_includes_all_project_apps_without_a_pal_prefix_filter(self):
        with tempfile.TemporaryDirectory() as directory:
            apps = Path(directory) / "projects/e2e/apps"
            for name in ("pal-json", "gizclaw", "atomic", "lua-link"):
                (apps / name).mkdir(parents=True)
            self.assertEqual(
                discover_apps(directory),
                ["atomic", "gizclaw", "lua-link", "pal-json"],
            )

    def test_excludes_invalid_aliases_and_board_or_product_target_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            apps = root / "projects/e2e/apps"
            for name in ("pal-core", "peripherals", "perpheral", "chip", "loader", "__pycache__"):
                (apps / name).mkdir(parents=True)
            (root / "projects/h2loader/targets/loader").mkdir(parents=True)
            (root / "boards/esp32s31_chip/tests").mkdir(parents=True)
            self.assertEqual(discover_apps(root), ["pal-core"])

    def test_does_not_report_success_for_a_missing_app_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "projects/e2e/apps"):
                discover_apps(directory)


if __name__ == "__main__":
    unittest.main()
