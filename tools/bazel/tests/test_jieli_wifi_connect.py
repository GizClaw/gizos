"""Check station connection wait semantics against the public PAL contract."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c"


class WifiConnectTest(unittest.TestCase):
    def test_async_and_elapsed_time_budget(self):
        source = SOURCE.read_text()
        begin = source.index("static int sta_connect(")
        end = source.index("static int wifi_stop(", begin)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_wifi.h"
static struct { h2_pal_wifi_sta_status_t sta; } wifi_state;
static uint32_t now;
static int sleeps, enter_rc, requests;
#define H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING 1
static int ensure_wifi_on(void) { return 0; }
static void post_sta_event(int type) { (void)type; }
static void update_sta_snapshot(void) {}
static uint32_t timer_get_ms(void) { return now; }
static int wifi_enter_sta_mode(const char *ssid, const char *password) {
    assert(strcmp(ssid, "test") == 0 && strcmp(password, "") == 0);
    ++requests; return enter_rc;
}
static void os_time_dly(unsigned ticks) {
    assert(ticks == 1); ++sleeps; now += 40;
}
'''
        main = r'''
int main(void) {
    h2_pal_wifi_sta_config_t config = {0};
    memcpy(config.ssid, "test", 4); config.ssid_len = 4;
    assert(sta_connect(NULL, &config, 0) == H2_PAL_OK);
    assert(requests == 1 && sleeps == 0);
    assert(sta_connect(NULL, &config, 50) == H2_PAL_ERR_TIMEOUT);
    assert(sleeps == 2 && now == 80);
    sleeps = 0; now = UINT32_MAX - 19;
    assert(sta_connect(NULL, &config, 50) == H2_PAL_ERR_TIMEOUT);
    assert(sleeps == 2 && now == 60);
    enter_rc = -1; sleeps = 0;
    assert(sta_connect(NULL, &config, 0) == H2_PAL_ERR_IO && sleeps == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-wifi-connect-") as directory:
            root = Path(directory)
            unit = root / "connect.c"
            unit.write_text(fixture + source[begin:end] + main)
            binary = root / "connect-test"
            subprocess.run(["cc", "-std=c11", "-I", str(ROOT / "libs/pal/include"),
                            str(unit), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
