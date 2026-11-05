import tempfile
import unittest
from pathlib import Path

from list_apps import discover_apps, discover_entries


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
            (root / "boards/espressif/mosaico/tests").mkdir(parents=True)
            self.assertEqual(discover_apps(root), ["pal-core"])

    def test_lists_single_and_pair_firmware_entries_separately(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "projects/e2e/targets"
            for kind, app, board in (
                ("single", "pal-core", "h106_zero_esp_v3_0"),
                ("pair", "lua-link", "h200_esp32s3"),
            ):
                entry = root / kind / app / "haivivi" / board
                entry.mkdir(parents=True)
                (entry / "BUILD.bazel").write_text("")
            (root / "cc_test/pal-core").mkdir(parents=True)
            (root / "single/not-built/haivivi/placeholder").mkdir(parents=True)
            self.assertEqual(discover_entries(directory, "single"), [{
                "app": "pal-core",
                "manufacturer": "haivivi",
                "board": "h106_zero_esp_v3_0",
                "path": "projects/e2e/targets/single/pal-core/haivivi/h106_zero_esp_v3_0",
            }])
            self.assertEqual(discover_entries(directory, "pair"), [{
                "app": "lua-link",
                "manufacturer": "haivivi",
                "board": "h200_esp32s3",
                "path": "projects/e2e/targets/pair/lua-link/haivivi/h200_esp32s3",
            }])

    def test_rejects_unknown_or_missing_execution_categories(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "single or pair"):
                discover_entries(directory, "unit")
            with self.assertRaisesRegex(ValueError, "targets/pair"):
                discover_entries(directory, "pair")

    def test_does_not_report_success_for_a_missing_app_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "projects/e2e/apps"):
                discover_apps(directory)


if __name__ == "__main__":
    unittest.main()
