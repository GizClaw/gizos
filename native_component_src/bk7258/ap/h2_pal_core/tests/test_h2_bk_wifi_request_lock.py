"""Compile and execute production Wi-Fi connect/status/IPv6 lock paths."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[5]
PACKAGE = ROOT / "native_component_src/bk7258/ap/h2_pal_core"


def function(source, name):
    match = re.search(r"^static\s+[\w*]+\s+" + re.escape(name) +
                      r"\s*\([^;{]*\)\s*\{", source, re.M)
    if match is None:
        raise AssertionError("missing production function: " + name)
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class RequestLock(unittest.TestCase):
    def test_repeat_connect_with_refresh_worker(self):
        source = (PACKAGE / "src/h2_bk_platform_wifi.c").read_text()
        names = [
            "h2_bk_wifi_request_lock", "h2_bk_wifi_request_unlock",
            "h2_bk_wifi_event_lock", "h2_bk_wifi_event_unlock",
            "h2_bk_wifi_status_lock", "h2_bk_wifi_status_unlock",
            "h2_bk_wifi_strnlen", "h2_bk_wifi_store_sta_status",
            "h2_bk_wifi_load_sta_status", "h2_bk_wifi_state",
            "h2_bk_wifi_fill_sta_ip", "h2_bk_wifi_copy_sta_config",
            "h2_bk_wifi_sta_is_connected", "h2_bk_wifi_sta_cached_connected",
        ]
        parts = [function(source, name) for name in names]
        # Preserve the complete status implementation and public entry,
        # including a private lock-safe reader when present. This also permits
        # executing the pre-fix source as a controlled deadlock regression.
        start = source.index("static int h2_bk_wifi_sta_",
                             source.index("static void h2_bk_wifi_copy_scan_entry"))
        end = source.index("\nstatic int h2_bk_wifi_sta_scan(", start)
        parts.append(source[start:end])
        parts.append(function(source, "h2_bk_wifi_sta_connect"))
        start = source.index("#if LWIP_IPV6\ntypedef struct h2_bk_wifi_ipv6_sync_call")
        end = source.index("#if LWIP_IPV6\nstatic void h2_bk_wifi_ipv6_watch", start)
        parts.append(source[start:end])
        probe = (PACKAGE / "tests/h2_bk_wifi_request_lock_probe.c").read_text()
        compiler = shutil.which("cc")
        self.assertTrue(compiler, "a native C compiler is required")
        with tempfile.TemporaryDirectory(prefix="h2-bk-wifi-lock-") as directory:
            path = Path(directory)
            unit, binary = path / "probe.c", path / "probe"
            unit.write_text(probe.replace("/* PRODUCTION_FUNCTIONS */",
                                          "\n\n".join(parts)))
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       # The SDK RPC ABI is 32-bit; this host fixture keeps the
                       # allocated buffer separately instead of decoding it.
                       "-Wno-pointer-to-int-cast", "-pthread",
                       "-I" + str(ROOT / "libs/pal/include"),
                       "-I" + str(PACKAGE / "src"),
                       "-I" + str(ROOT / "native_component_src/bk7258/shared"),
                       str(unit), "-o", str(binary)]
            build = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
