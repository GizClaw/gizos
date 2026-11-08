import tempfile
import unittest
from pathlib import Path

from list_apps import discover_apps, discover_entries


class InventoryTest(unittest.TestCase):
    def test_includes_suites_from_each_category(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "projects/testing"
            for name in ("unit/pal-json", "integration/gizclaw", "unit/atomic", "e2e/lua_link", "benchmark/webrtc-performance"):
                (root / name).mkdir(parents=True)
            self.assertEqual(discover_apps(directory), ["atomic", "gizclaw", "lua_link", "pal-json", "webrtc-performance"])

    def test_excludes_helpers_and_artifact_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "projects/testing"
            for name in ("unit/pal-core", "unit/peripherals", "unit/__pycache__", "libs/host_runtime", "targets/cc_test/pal-core"):
                (root / name).mkdir(parents=True)
            self.assertEqual(discover_apps(directory), ["pal-core"])

    def test_lists_category_and_role_without_test_helper_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "projects/testing/targets/h2_loader_tar_zlib"
            for name in ("unit/pal-core/haivivi/h106_zero_esp_v3_0", "e2e/lua_link/host/espressif/esp32s3_devkitc_1"):
                entry = root / name
                entry.mkdir(parents=True)
                (entry / "BUILD.bazel").write_text('h2loader_tar_zlib(name = "package")\n')
            helper = root / "unit/pal-audio/tests"
            helper.mkdir(parents=True)
            (helper / "BUILD.bazel").write_text('cc_test(name = "test")\n')
            self.assertEqual(discover_entries(directory, "unit"), [{
                "app": "pal-core", "manufacturer": "haivivi", "board": "h106_zero_esp_v3_0",
                "path": "projects/testing/targets/h2_loader_tar_zlib/unit/pal-core/haivivi/h106_zero_esp_v3_0",
            }])
            self.assertEqual(discover_entries(directory, "e2e"), [{
                "app": "lua_link", "role": "host", "manufacturer": "espressif", "board": "esp32s3_devkitc_1",
                "path": "projects/testing/targets/h2_loader_tar_zlib/e2e/lua_link/host/espressif/esp32s3_devkitc_1",
            }])
            with self.assertRaisesRegex(ValueError, "Testing category"):
                discover_entries(directory, "mfg")

    def test_rejects_unknown_or_missing_categories(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "Testing category"):
                discover_entries(directory, "single")
            with self.assertRaisesRegex(ValueError, "h2_loader_tar_zlib/e2e"):
                discover_entries(directory, "e2e")

    def test_reports_missing_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "projects/testing"):
                discover_apps(directory)


if __name__ == "__main__":
    unittest.main()
