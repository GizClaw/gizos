"""Pin SDK supplicant extraction and bounded association."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
BOARD = ROOT / "boards/jieli_ac791n_devkit/ac791n"


class WifiLinkTest(unittest.TestCase):
    def test_real_supplicant_is_extracted(self):
        source = (BOARD / "layouts/h2loader/project.mk").read_text()
        self.assertLess(source.index("cpu/wl82/liba/wpasupplicant.a"),
                        source.index("cpu/wl82/liba/wl_wifi.a"))
        self.assertNotIn("hostapd_and_wpasupplicant.a", source)
        self.assertRegex(source, r"LFLAGS\s*\+=\s*--undefined=wpa_supplicant_get_state")

    def test_apps_register_sdk_wifi_tasks(self):
        removed_policy = "_".join(("wifi", "task", "policies")) + ".bzl"
        self.assertFalse((BOARD / removed_policy).exists())
        rows = [
            "tcpip_thread 16 800 0",
            "tasklet 10 1400 0",
            "RtmpMlmeTask 17 700 0",
            "RtmpCmdQTask 17 300 0",
            "wl_rx_irq_thread 5 256 0",
        ]
        for name in ("display", "button", "touch", "audio-system", "mp4-player"):
            with self.subTest(app=name):
                build = (ROOT / "projects/example/targets/h2loader_tar_zlib" /
                         name / "jieli_ac791n_devkit/BUILD.bazel").read_text()
                self.assertNotIn(removed_policy, build)
                for row in rows:
                    self.assertIn('"' + row + '"', build)

    def test_association_has_sdk_timeout(self):
        source = (BOARD / "src/h2_jieli_ac791n_devkit_wifi.c").read_text()
        source = source[source.index("static int sta_connect("):]
        self.assertLess(source.index("wifi_set_sta_connect_timeout("),
                        source.index("wifi_enter_sta_mode("))
        self.assertRegex(source, r"timeout_ms\s*==\s*0u\s*\?\s*30")
        self.assertIn("timeout_ms / 1000u", source)
        self.assertIn("timeout_ms % 1000u != 0u", source)


if __name__ == "__main__":
    unittest.main()
