"""Exercise production BK BLE Host functions with persistent SDK service tables."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

COMPONENT = Path(__file__).resolve().parents[1]
ROOT = Path(__file__).resolve().parents[5]


def function_source(text, name):
    match = re.search(r"static [^\n]+\b" + name + r"\([^;]+?\) \{", text)
    if match is None:
        raise ValueError(f"Missing production function: {name}")
    return text[match.start():text.index("\n}", match.start()) + 2]


class BKBLEHostLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (COMPONENT / "src/h2_bk_platform_ble.c").read_text()
        functions = [
            "h2_bk_ble_clear_out_handle_ptrs",
            "h2_bk_ble_unregister_gatt_services",
            "h2_bk_ble_start_configured_services",
            "h2_bk_ble_start",
            "h2_bk_ble_stop",
        ]
        body = "\n\n".join(function_source(source, name) for name in functions)
        probe = (COMPONENT / "tests/ble_host_lifecycle_probe.c").read_text()
        cls.directory = tempfile.TemporaryDirectory(prefix="bk-ble-host-")
        cls.addClassCleanup(cls.directory.cleanup)
        unit = Path(cls.directory.name) / "probe.c"
        cls.binary = Path(cls.directory.name) / "probe"
        unit.write_text(probe.replace("/* PRODUCTION_FUNCTIONS */", body))
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "libs/pal/include"),
            "-I" + str(COMPONENT / "src"),
            str(unit), "-o", str(cls.binary),
        ], check=True)

    def run_scenario(self, scenario):
        result = subprocess.run(
            [str(self.binary), str(scenario)], capture_output=True,
            text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_restart_reuses_installed_service_tables(self):
        self.run_scenario(0)

    def test_stop_releases_borrowed_bindings(self):
        self.run_scenario(1)

    def test_failed_detach_retains_ownership_for_retry(self):
        self.run_scenario(2)


if __name__ == "__main__":
    unittest.main()
