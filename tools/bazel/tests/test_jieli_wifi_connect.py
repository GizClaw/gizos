"""Check station connection wait semantics against the public PAL contract."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c"


# Pinned SDK include_lib/net/wifi/wifi_connect.h (eb04f1966cf2).
SDK_TYPES = r'''
typedef unsigned char u8;
enum WIFI_MODE { STA_MODE = 1, AP_MODE, P2P_MODE, SMP_CFG_MODE, MP_TEST_MODE, NONE_MODE };
enum P2P_ROLE { P2P_GC_MODE = 1, P2P_GO_MODE };
struct wifi_store_info {
  enum WIFI_MODE mode;
  u8 pwd[2][64];
  u8 ssid[2][33];
  enum P2P_ROLE p2p_role;
  u8 sta_cnt;
  u8 connect_best_network;
} __attribute__((packed));
'''


class WifiConnectTest(unittest.TestCase):
    def test_ap_elapsed_time_budget(self):
        source = SOURCE.read_text()
        begin = source.index("static int ap_start(")
        end = source.index("static int ap_stop(", begin)
        fixture = r'''
#include <assert.h>

static unsigned fake_state_gate;
static inline void wifi_state_lock(void) {
    assert(fake_state_gate == 0);
    fake_state_gate = 1;
}
static inline void wifi_state_unlock(void) {
    assert(fake_state_gate == 1);
    fake_state_gate = 0;
}

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
    assert(fake_state_gate == 0);
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
            unit.write_text(SDK_TYPES + fixture + source[begin:end] + main)
            binary = root / "ap-test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "libs/pal/include"),
                            str(unit), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_async_and_elapsed_time_budget(self):
        source = SOURCE.read_text()
        begin = source.index("static int sta_connect(")
        end = source.index("static int wifi_stop(", begin)
        fixture = r'''
#include <assert.h>

static unsigned fake_state_gate;
static inline void wifi_state_lock(void) {
    assert(fake_state_gate == 0);
    fake_state_gate = 1;
}
static inline void wifi_state_unlock(void) {
    assert(fake_state_gate == 1);
    fake_state_gate = 0;
}

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
static int wifi_is_on(void) { return 1; }
static int wifi_set_default_mode(struct wifi_store_info *parm, char force, char store) {
    (void)parm; (void)force; (void)store;
    assert(0); return -1;
}
static uint32_t wifi_sta_generation;
static void wifi_set_sta_connect_timeout(int seconds) { assert(seconds > 0); }
static void post_sta_event(int type, const h2_pal_wifi_sta_status_t *status) {
    assert(fake_state_gate == 0);
    assert(status->state == wifi_state.sta.state);
    last_event = type;
}
static uint32_t timer_get_ms(void) { return now; }
static int wifi_enter_sta_mode(const char *ssid, const char *password) {
    assert(fake_state_gate == 0);
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
    assert(sta_connect(NULL, &config, 0) == H2_PAL_OK && sleeps == 0);
    assert(wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_CONNECTING);
    assert(last_event == H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING);
    assert(wifi_state.sta.ip_valid == 0 && wifi_state.sta.disconnect_reason == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-wifi-connect-") as directory:
            root = Path(directory)
            unit = root / "connect.c"
            unit.write_text(SDK_TYPES + fixture + source[begin:end] + main)
            binary = root / "connect-test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "libs/pal/include"),
                            str(unit), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


STUB = SDK_TYPES + r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_IO -4
#define H2_PAL_ERR_TIMEOUT -6
#define H2_PAL_WIFI_SSID_MAX 32
#define H2_PAL_WIFI_PASSWORD_MAX 64
#define H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING 1
#define H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED 2
typedef enum {
  H2_PAL_WIFI_STA_STATE_UNKNOWN = 0,
  H2_PAL_WIFI_STA_STATE_IDLE = 1,
  H2_PAL_WIFI_STA_STATE_SCANNING = 2,
  H2_PAL_WIFI_STA_STATE_CONNECTING = 3,
  H2_PAL_WIFI_STA_STATE_CONNECTED = 4,
  H2_PAL_WIFI_STA_STATE_GOT_IP = 5,
  H2_PAL_WIFI_STA_STATE_DISCONNECTED = 6,
  H2_PAL_WIFI_STA_STATE_FAILED = 7,
} h2_pal_wifi_sta_state_t;
typedef struct {
  char ssid[33], password[65];
  size_t ssid_len, password_len;
} h2_pal_wifi_sta_config_t;
typedef struct {
  h2_pal_wifi_sta_state_t state;
  char ssid[33];
  size_t ssid_len;
  uint8_t ip_valid;
  int disconnect_reason;
} h2_pal_wifi_sta_status_t;
static struct { h2_pal_wifi_sta_status_t sta; } wifi_state;
static uint32_t wifi_sta_generation;
static uint32_t now_ms;
static int sdk_timeout, timeout_calls, connect_calls, delays;
static int events[8], event_count;
static int radio_on, default_rc, default_calls, ensure_calls, ensure_rc;
static int recorded_force, recorded_store;
static struct wifi_store_info recorded_default;
static h2_pal_wifi_sta_state_t next_state;
static char received_ssid[33], received_password[65];
static int h2_pal_wifi_settings_validate_sta_config(const h2_pal_wifi_sta_config_t *config) {
  assert(config && config->ssid_len <= 32 && config->password_len <= 64);
  return H2_PAL_OK;
}
static int wifi_is_on(void) { return radio_on; }
static int wifi_set_default_mode(struct wifi_store_info *parm, char force, char store) {
  assert(!radio_on && ensure_calls == 0 && timeout_calls == 1);
  recorded_default = *parm;
  recorded_force = force; recorded_store = store;
  ++default_calls;
  return default_rc;
}
static int ensure_wifi_on(void) {
  ++ensure_calls;
  if (!radio_on) assert(default_calls == 1 && default_rc == 0);
  if (ensure_rc == H2_PAL_OK) radio_on = 1;
  return ensure_rc;
}
static void wifi_state_lock(void) {}
static void wifi_state_unlock(void) {}
static void post_sta_event(int type, const h2_pal_wifi_sta_status_t *status) {
  assert(status->state == wifi_state.sta.state);
  assert(event_count < 8);
  events[event_count++] = type;
}
static void wifi_set_sta_connect_timeout(int seconds) {
  sdk_timeout = seconds; ++timeout_calls;
}
static int wifi_enter_sta_mode(const char *ssid, const char *password) {
  assert(radio_on && timeout_calls == 1);
  strcpy(received_ssid, ssid); strcpy(received_password, password);
  ++connect_calls;
  return -1;
}
static uint32_t timer_get_ms(void) { return now_ms; }
static void os_time_dly(unsigned ticks) {
  assert(ticks == 1u);
  now_ms += 100u;
  ++delays;
  if (next_state != H2_PAL_WIFI_STA_STATE_UNKNOWN) {
    wifi_state.sta.state = next_state;
  }
}
static void reset(void) {
  memset(&wifi_state, 0, sizeof(wifi_state));
  wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_IDLE;
  wifi_sta_generation = now_ms = 0;
  sdk_timeout = timeout_calls = connect_calls = delays = event_count = 0;
  next_state = H2_PAL_WIFI_STA_STATE_UNKNOWN;
  radio_on = 1;
  default_rc = default_calls = ensure_calls = ensure_rc = 0;
  recorded_force = recorded_store = -1;
  memset(&recorded_default, 0, sizeof(recorded_default));
  memset(received_ssid, 0, sizeof(received_ssid));
  memset(received_password, 0, sizeof(received_password));
}
static void check_start(int expected_timeout) {
  assert(connect_calls == 1 && timeout_calls == 1);
  assert(default_calls == 0);
  assert(sdk_timeout == expected_timeout);
  assert(strcmp(received_ssid, "test-network") == 0);
  assert(strcmp(received_password, "placeholder") == 0);
  assert(event_count == 1);
  assert(events[0] == H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING);
  assert(wifi_sta_generation == 1);
}
'''
MAIN = r'''
int main(void) {
  const h2_pal_wifi_sta_config_t config = {
    .ssid = "test-network", .ssid_len = 12,
    .password = "placeholder", .password_len = 11,
  };
  reset();
  int result = sta_connect(NULL, &config, 0u);
  if (result != H2_PAL_OK) {
    fprintf(stderr, "case (a): expected H2_PAL_OK, got %d\n", result);
    return 1;
  }
  assert(wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_CONNECTING);
  assert(wifi_state.sta.disconnect_reason == 0);
  assert(delays == 0 && now_ms == 0);
  check_start(30);

  reset(); next_state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  assert(sta_connect(NULL, &config, 2500u) == H2_PAL_OK);
  assert(wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_GOT_IP);
  assert(delays == 1);
  check_start(3);

  reset(); next_state = H2_PAL_WIFI_STA_STATE_FAILED;
  assert(sta_connect(NULL, &config, 1000u) == H2_PAL_ERR_IO);
  assert(wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_FAILED);
  assert(delays == 1);
  check_start(1);

  reset();
  assert(sta_connect(NULL, &config, 1000u) == H2_PAL_ERR_TIMEOUT);
  assert(wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_CONNECTING);
  assert(now_ms == 1000u && delays == 10);
  check_start(1);

  reset(); radio_on = 0;
  assert(sta_connect(NULL, &config, 0u) == H2_PAL_OK);
  assert(default_calls == 1 && ensure_calls == 1 && connect_calls == 0);
  assert(timeout_calls == 1 && sdk_timeout == 30);
  assert(recorded_default.mode == STA_MODE);
  assert(strcmp((char *)recorded_default.ssid[0], "test-network") == 0);
  assert(strcmp((char *)recorded_default.pwd[0], "placeholder") == 0);
  assert(recorded_default.connect_best_network == 0);
  assert(recorded_default.sta_cnt == 0 && recorded_default.p2p_role == 0);
  assert(recorded_default.ssid[1][0] == 0 && recorded_default.pwd[1][0] == 0);
  assert(recorded_force == 1 && recorded_store == 0);
  assert(event_count == 1 && events[0] == H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING);
  assert(wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_CONNECTING);

  reset(); radio_on = 0; default_rc = -1;
  assert(sta_connect(NULL, &config, 0u) == H2_PAL_ERR_IO);
  assert(default_calls == 1 && ensure_calls == 0 && connect_calls == 0);
  assert(timeout_calls == 1 && sdk_timeout == 30);
  assert(wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_FAILED);
  assert(wifi_state.sta.disconnect_reason == -1 && wifi_state.sta.ip_valid == 0);
  assert(event_count == 2 && events[1] == H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED);

  reset(); radio_on = 0; next_state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  assert(sta_connect(NULL, &config, 2500u) == H2_PAL_OK);
  assert(default_calls == 1 && connect_calls == 0 && sdk_timeout == 3);
  assert(delays == 1);

  reset(); radio_on = 0; ensure_rc = H2_PAL_ERR_IO;
  assert(sta_connect(NULL, &config, 0u) == H2_PAL_ERR_IO);
  assert(default_calls == 1 && ensure_calls == 1 && connect_calls == 0);
  return 0;
}
'''


def run_connect_fixture(source):
    begin = source.index("static int sta_connect(")
    end = source.index("static int wifi_stop(", begin)
    with tempfile.TemporaryDirectory(prefix="h2-wifi-connect-") as directory:
        test = Path(directory) / "test.c"
        test.write_text(STUB + source[begin:end] + MAIN)
        binary = Path(directory) / "test"
        subprocess.run(["cc", "-std=c11", "-Werror", str(test), "-o", str(binary)],
                       check=True, timeout=60)
        return subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)


class NonblockingWifiConnectTest(unittest.TestCase):
    def test_nonblocking_start_and_event_outcomes(self):
        result = run_connect_fixture(SOURCE.read_text())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
