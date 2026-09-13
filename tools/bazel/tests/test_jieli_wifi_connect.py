"""Check station connection wait semantics against the public PAL contract."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c"


class WifiConnectTest(unittest.TestCase):
    def test_ap_elapsed_time_budget(self):
        source = SOURCE.read_text()
        begin = source.index("static int ap_start(")
        end = source.index("static int ap_stop(", begin)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_wifi.h"
static struct { h2_pal_wifi_ap_status_t ap; } wifi_state;
static uint32_t now;
static int sleeps, starts, stopped, configured, stop_rc, config_rc, enter_rc;
static unsigned applied_channel, applied_max, applied_hidden;
static int wifi_stop(void) { ++stopped; return stop_rc; }
static int h2_jieli_wifi_configure_ap(unsigned ch, unsigned max, unsigned hidden) {
    assert(stopped > configured); ++configured;
    applied_channel = ch; applied_max = max; applied_hidden = hidden;
    return config_rc;
}
static int ensure_wifi_on(void) { assert(configured > starts); ++starts; return 0; }
static uint32_t timer_get_ms(void) { return now; }
static int wifi_get_channel(void) { return 1; }
static int wifi_enter_ap_mode(char *ssid, char *password) {
    (void)ssid; (void)password; return enter_rc;
}
static void os_time_dly(unsigned ticks) {
    assert(ticks == 1); ++sleeps; now += 40;
}
'''
        main = r'''
int main(void) {
    h2_pal_wifi_ap_config_t config = {0};
    memcpy(config.ssid, "test", 4); config.ssid_len = 4;
    config.security = H2_PAL_WIFI_SECURITY_OPEN;
    config.max_clients = 6;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_UNSUPPORTED);
    assert(starts == 0);
    config.max_clients = 0;
    memcpy(config.password, "password", 8); config.password_len = 8;
    config.security = H2_PAL_WIFI_SECURITY_WPA3;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_UNSUPPORTED);
    assert(starts == 0);
    config.security = H2_PAL_WIFI_SECURITY_OPEN;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_INVALID_ARG);
    assert(starts == 0);
    config.password_len = 0;
    config.channel = 15;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_INVALID_ARG);
    assert(stopped == 0);
    config.channel = 6; config.max_clients = 4; config.hidden = 1;
    stop_rc = H2_PAL_ERR_IO;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_IO);
    assert(configured == 0 && starts == 0);
    stop_rc = 0; config_rc = -1;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_IO);
    assert(starts == 0);
    config_rc = 0;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_TIMEOUT);
    assert(applied_channel == 6 && applied_max == 4 && applied_hidden == 1);
    assert(wifi_state.ap.max_clients == 4 && wifi_state.ap.hidden == 1);
    assert(sleeps == 2 && now == 80);
    sleeps = 0; now = UINT32_MAX - 19;
    config.channel = 0; config.max_clients = 0; config.hidden = 0;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_TIMEOUT);
    assert(applied_channel == 1 && applied_max == 2 && applied_hidden == 0);
    assert(sleeps == 2 && now == 60);
    enter_rc = -1; sleeps = 0;
    assert(ap_start(NULL, &config, 50) == H2_PAL_ERR_IO);
    assert(wifi_state.ap.state == H2_PAL_WIFI_AP_STATE_UNKNOWN && sleeps == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-wifi-ap-") as directory:
            root = Path(directory)
            unit = root / "ap.c"
            unit.write_text(fixture + source[begin:end] + main)
            binary = root / "ap-test"
            subprocess.run(["cc", "-std=c11", "-I", str(ROOT / "libs/pal/include"),
                            str(unit), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)

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
#define H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED 2
static int last_event;
static int ensure_wifi_on(void) { return 0; }
static void post_sta_event(int type) { last_event = type; }
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
    assert(wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_FAILED);
    assert(last_event == H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED);
    assert(wifi_state.sta.ip_valid == 0 && wifi_state.sta.disconnect_reason == -1);
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
