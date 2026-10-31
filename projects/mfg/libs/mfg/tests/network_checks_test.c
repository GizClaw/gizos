#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fake {
  h2_mfg_t m;
  h2_runtime_t rt;
  h2_mfg_profile_t profile;
  h2_mfg_step_t step;
  h2_pal_ble_host_api_t ble;
  h2_pal_wifi_sta_api_t wifi;
  h2_pal_wifi_sta_status_t status;
  h2_pal_wifi_settings_api_t settings;
  h2_pal_sync_api_t wifi_sync;
  int wifi_operation_locked;
  h2_pal_wifi_sta_config_t saved_config, connected_config;
  int saved_error, connect_error, disconnect_error;
  unsigned connects, disconnects, saved_reads;
  uint32_t connect_timeout;
  h2_pal_ble_scan_result_fn callback;
  void* callback_user;
  int host_started, scanning, start_error, scan_error, stop_error, wifi_error;
  int callback_on_start, callback_on_stop;
  unsigned starts, scans, stops;
} fake_t;

static void report(fake_t* f, unsigned id, const char* name, size_t n,
                   int rssi) {
  h2_pal_ble_scan_result_t result = {
      .addr = {.value = {(uint8_t)id, (uint8_t)(id >> 8)},
               .type = H2_PAL_BLE_ADDR_TYPE_PUBLIC},
      .rssi = rssi,
      .local_name = name,
      .local_name_len = n};
  assert(f->callback && f->m.scan_mutex);
  assert(!f->callback(f->callback_user, &result));
}

static int start(void* user) {
  fake_t* f = user;
  ++f->starts;
  if (f->start_error) return f->start_error;
  f->host_started = 1;
  return 0;
}
static int start_scan(void* user, const h2_pal_ble_scan_params_t* params,
                      h2_pal_ble_scan_result_fn callback, void* callback_user) {
  fake_t* f = user;
  assert(f->host_started && !f->scanning);
  assert(params->mode == H2_PAL_BLE_SCAN_MODE_ACTIVE);
  ++f->scans;
  f->scanning = 1;
  f->callback = callback;
  f->callback_user = callback_user;
  if (f->callback_on_start) report(f, 1, "inline", 6, -50);
  return f->scan_error;
}
static int stop_scan(void* user) {
  fake_t* f = user;
  assert(f->scanning && f->m.scan_mutex);
  ++f->stops;
  /* A provider may finish one callback during stop while session mutex is held.
   */
  if (f->callback_on_stop) report(f, 2, "last", 4, -40);
  if (f->stop_error) return f->stop_error;
  f->scanning = 0;
  f->callback = NULL;
  f->callback_user = NULL;
  return 0;
}
static int wifi_status(void* user, h2_pal_wifi_sta_status_t* status) {
  fake_t* f = user;
  if (f->profile.wifi_test_config)
    assert(f->m.wifi_lock_owned && f->wifi_operation_locked);
  if (f->wifi_error) return f->wifi_error;
  *status = f->status;
  return 0;
}
static const h2_pal_ble_vtable_t ble_ops = {
    .start = start, .start_scan = start_scan, .stop_scan = stop_scan};
static int wifi_try_lock(void* user, h2_pal_mutex_t* mutex) {
  fake_t* f = user;
  assert(mutex == (h2_pal_mutex_t*)f);
  if (f->wifi_operation_locked) return H2_PAL_ERR_WOULD_BLOCK;
  f->wifi_operation_locked = 1;
  return 0;
}
static int wifi_unlock(void* user, h2_pal_mutex_t* mutex) {
  fake_t* f = user;
  assert(mutex == (h2_pal_mutex_t*)f && f->wifi_operation_locked);
  f->wifi_operation_locked = 0;
  return 0;
}
static const h2_pal_sync_vtable_t wifi_sync_ops = {
    .try_lock_mutex = wifi_try_lock, .unlock_mutex = wifi_unlock};
static void wifi_locked(fake_t* f) {
  assert(f->m.wifi_lock_owned && f->wifi_operation_locked);
}
static int wifi_connect(void* user, const h2_pal_wifi_sta_config_t* config,
                        uint32_t timeout) {
  fake_t* f = user;
  wifi_locked(f);
  ++f->connects;
  f->connected_config = *config;
  f->connect_timeout = timeout;
  memset(&f->status, 0, sizeof(f->status));
  f->status.state = H2_PAL_WIFI_STA_STATE_CONNECTING;
  memcpy(f->status.ssid, config->ssid, config->ssid_len);
  f->status.ssid_len = config->ssid_len;
  memcpy(f->status.bssid, config->bssid, sizeof(config->bssid));
  f->status.bssid_set = config->bssid_set;
  return f->connect_error;
}
static int wifi_disconnect(void* user) {
  fake_t* f = user;
  wifi_locked(f);
  ++f->disconnects;
  if (f->disconnect_error) return f->disconnect_error;
  f->status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  return 0;
}
static int wifi_saved(void* user, h2_pal_wifi_sta_config_t* config) {
  fake_t* f = user;
  wifi_locked(f);
  ++f->saved_reads;
  if (f->saved_error) return f->saved_error;
  *config = f->saved_config;
  return 0;
}
static const h2_pal_wifi_sta_vtable_t wifi_ops = {
    .get_status = wifi_status,
    .connect = wifi_connect,
    .disconnect = wifi_disconnect};
static const h2_pal_wifi_settings_vtable_t settings_ops = {
    .get_saved_sta_config = wifi_saved};
static const h2_pal_wifi_sta_config_t test_network = {
    .ssid = "fixture-test",
    .ssid_len = sizeof("fixture-test") - 1u,
    .password = "test-password",
    .password_len = sizeof("test-password") - 1u};
static const h2_pal_wifi_sta_config_t previous_network = {
    .ssid = "management",
    .ssid_len = sizeof("management") - 1u,
    .password = "previous-password",
    .password_len = sizeof("previous-password") - 1u,
    .bssid = {1, 2, 3, 4, 5, 6},
    .bssid_set = 1,
    .channel = 6};

static void setup(fake_t* f, h2_mfg_check_t check) {
  memset(f, 0, sizeof(*f));
  f->ble = (h2_pal_ble_host_api_t){.user = f, .vtable = &ble_ops};
  f->wifi = (h2_pal_wifi_sta_api_t){.user = f, .vtable = &wifi_ops};
  f->settings =
      (h2_pal_wifi_settings_api_t){.user = f, .vtable = &settings_ops};
  f->step = (h2_mfg_step_t){.name = "network", .check = check};
  f->profile = (h2_mfg_profile_t){.board = "test",
                                  .revision = 1,
                                  .steps = &f->step,
                                  .total = 1,
                                  .ble_scan_only = 1,
                                  .wifi_manual_ipv4 = 1,
                                  .scan_window_ms = 10000};
  f->rt = (h2_runtime_t){.ble_host = &f->ble,
                         .wifi_sta = &f->wifi,
                         .wifi_settings = &f->settings,
                         .sync = h2_desktop_platform_sync_api(),
                         .mem = h2_desktop_platform_default_allocator()};
  f->m = (h2_mfg_t){.rt = &f->rt,
                    .profile = &f->profile,
                    .model = {.profile = &f->profile, .summary = {.total = 1}}};
  h2_pal_mutex_config_t config = {.name = "network_test",
                                  .allocator = f->rt.mem};
  assert(h2_pal_mutex_create(f->rt.sync, &config, &f->m.mutex) == 0);
  f->wifi_sync = (h2_pal_sync_api_t){.user = f, .vtable = &wifi_sync_ops};
  f->m.wifi_sync = &f->wifi_sync;
  f->m.wifi_mutex = (h2_pal_mutex_t*)f;
}
static int tick(fake_t* f, uint64_t now) {
  const char* action = NULL;
  int rc = h2_mfg_hardware_tick(&f->m, now, &action);
  assert(!action &&
         f->m.model.summary.step_status[0] == H2_LOADER_MFG_STEP_UNTESTED);
  return rc;
}
static void cleanup(fake_t* f) {
  assert(h2_mfg_hardware_leave(&f->m) == 0);
  assert(!f->m.wifi_config_owned && !f->m.wifi_lock_owned);
  assert(!f->wifi_operation_locked);
  assert(h2_pal_mutex_destroy(f->rt.sync, f->m.mutex) == 0);
}

static void bounded_copied_scan_and_stop(void) {
  fake_t f;
  setup(&f, H2_MFG_BLE);
  f.callback_on_start = f.callback_on_stop = 1;
  assert(h2_pal_mutex_lock(f.rt.sync, f.m.mutex) == 0);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(f.starts == 1 && f.scans == 1 && f.m.scan_count == 1);
  char name[] = "borrowed name";
  report(&f, 1, name, strlen(name), -60);
  name[0] = 'X';
  assert(f.m.scan_count == 1 &&
         !strcmp(f.m.scan_devices[0].name, "borrowed name"));
  report(&f, 1, NULL, 0, -30);
  assert(f.m.scan_devices[0].rssi == -30);
  assert(!strcmp(f.m.scan_devices[0].name, "borrowed name"));
  char long_name[80];
  memset(long_name, 'N', sizeof(long_name));
  report(&f, 3, long_name, sizeof(long_name), -70);
  assert(strlen(f.m.scan_devices[1].name) == 31);
  report(&f, 4, NULL, 0, -80);
  assert(f.m.scan_devices[2].name[0] == '\0');
  assert(f.m.scan_devices[2].address.value[0] == 4);
  for (unsigned i = 5; i < 100; ++i) report(&f, i, NULL, 0, -90);
  assert(f.m.scan_count == 32);
  report(&f, 1, NULL, 0, -20);
  assert(f.m.scan_devices[0].rssi == -20);
  assert(tick(&f, 9999) == 0 && !f.m.model.ready && !f.stops);
  assert(tick(&f, 10000) == 0 && f.m.model.ready && f.stops == 1);
  assert(f.m.scan_done && !f.m.scan_started);
  assert(tick(&f, 20000) == 0 && f.stops == 1);
  assert(h2_mfg_hardware_leave(&f.m) == 0 && !f.m.scan_mutex);
  assert(h2_pal_mutex_unlock(f.rt.sync, f.m.mutex) == 0);
  cleanup(&f);
}

static void scan_errors_keep_real_ownership(void) {
  fake_t f;
  setup(&f, H2_MFG_BLE);
  f.start_error = H2_PAL_ERR_IO;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_IO);
  assert(!f.scans && f.m.scan_error == H2_PAL_ERR_IO);
  cleanup(&f);

  setup(&f, H2_MFG_BLE);
  f.scan_error = H2_PAL_ERR_IO;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_IO);
  assert(f.m.scan_started && f.m.scan_mutex);
  f.stop_error = H2_PAL_ERR_BUSY;
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
  assert(f.m.scan_started && f.m.scan_mutex &&
         f.m.scan_error == H2_PAL_ERR_BUSY);
  report(&f, 10, "still owned", 11, -50);
  f.stop_error = 0;
  cleanup(&f);

  setup(&f, H2_MFG_BLE);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(tick(&f, 9999) == 0 && !f.m.model.ready);
  assert(tick(&f, 10000) == H2_PAL_ERR_TIMEOUT && !f.m.model.ready);
  assert(f.m.scan_done && !f.m.scan_count);
  cleanup(&f);

  setup(&f, H2_MFG_BLE);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  report(&f, 1, NULL, 0, -50);
  f.stop_error = H2_PAL_ERR_BUSY;
  assert(tick(&f, 10000) == H2_PAL_ERR_BUSY && !f.m.model.ready);
  assert(f.m.scan_started && f.m.scan_mutex);
  f.stop_error = 0;
  cleanup(&f);

  setup(&f, H2_MFG_BLE);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  report(&f, 1, NULL, 5, -50);
  assert(tick(&f, 20) == H2_PAL_ERR_FORMAT && !f.m.model.ready);
  cleanup(&f);
}

static void manual_wifi_requires_current_ipv4(void) {
  fake_t f;
  setup(&f, H2_MFG_WIFI);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  f.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  f.status.ip.ip6_valid = 1;
  f.status.ip.ip6[0] = 0x20;
  f.status.ip.ip6[1] = 0x01;
  assert(h2_pal_wifi_sta_status_has_ip(&f.status));
  assert(tick(&f, 20) == 0 && !f.m.model.ready);
  f.status.ip_valid = 1;
  assert(tick(&f, 40) == 0 && !f.m.model.ready);
  f.status.ip.ip4 = 0x0101a8c0;
  f.status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  assert(tick(&f, 60) == 0 && !f.m.model.ready);
  f.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  assert(tick(&f, 80) == 0 && f.m.model.ready);
  assert(f.m.wifi_status.ip.ip4 == f.status.ip.ip4);
  f.wifi_error = H2_PAL_ERR_IO;
  assert(tick(&f, 100) == H2_PAL_ERR_IO && !f.m.model.ready);
  cleanup(&f);
}

static void wifi_available(fake_t* f) {
  assert(!f->m.wifi_lock_owned);
  assert(h2_pal_mutex_try_lock(f->m.wifi_sync, f->m.wifi_mutex) == 0);
  assert(h2_pal_mutex_unlock(f->m.wifi_sync, f->m.wifi_mutex) == 0);
}

static void configured_wifi_connects_and_restores(void) {
  fake_t f;
  setup(&f, H2_MFG_WIFI);
  f.saved_config = previous_network;
  memcpy(f.status.ssid, previous_network.ssid, previous_network.ssid_len);
  f.status.ssid_len = previous_network.ssid_len;
  memcpy(f.status.bssid, previous_network.bssid, sizeof(f.status.bssid));
  f.status.bssid_set = 1;
  f.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  f.profile.wifi_test_config = &test_network;
  f.profile.wifi_timeout_ms = 1000;
  assert(h2_mfg_hardware_enter(&f.m, 100) == 0);
  assert(f.connects == 1 && f.saved_reads == 1);
  assert(f.connected_config.ssid_len == sizeof("fixture-test") - 1u);
  assert(!memcmp(f.connected_config.ssid, test_network.ssid,
                 test_network.ssid_len));
  assert(f.connected_config.password_len == test_network.password_len &&
         !memcmp(f.connected_config.password, test_network.password,
                 test_network.password_len));
  assert(f.connect_timeout == 1000 && f.m.wifi_config_owned &&
         f.m.wifi_previous_valid);
  wifi_available(&f);
  assert(tick(&f, 101) == 0 && !f.m.model.ready);
  f.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  f.status.ip.ip6_valid = 1;
  f.status.ip.ip6[0] = 0x20;
  f.status.ip.ip6[1] = 1;
  assert(tick(&f, 120) == 0 && !f.m.model.ready);
  f.status.ip_valid = 1;
  assert(tick(&f, 140) == 0 && !f.m.model.ready);
  f.status.ip.ip4 = 0xc0000201;
  assert(tick(&f, 160) == 0 && f.m.model.ready);
  wifi_available(&f);
  assert(h2_mfg_hardware_leave(&f.m) == 0);
  assert(f.disconnects == 1 && f.connects == 2);
  assert(!memcmp(&f.connected_config, &previous_network,
                 sizeof(previous_network)));
  assert(!f.m.wifi_previous.ssid_len && !f.m.wifi_previous.password_len);
  assert(!f.m.wifi_previous_valid && !f.m.wifi_config_owned);
  cleanup(&f);
  assert(f.disconnects == 1 && f.connects == 2);
}

static void configured_wifi_restoration_retries(void) {
  fake_t f;
  setup(&f, H2_MFG_WIFI);
  f.profile.wifi_test_config = &test_network;
  f.saved_config = previous_network;
  memcpy(f.status.ssid, previous_network.ssid, previous_network.ssid_len);
  f.status.ssid_len = previous_network.ssid_len;
  memcpy(f.status.bssid, previous_network.bssid, sizeof(f.status.bssid));
  f.status.bssid_set = 1;
  f.status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  f.wifi_error = H2_PAL_ERR_IO;
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_IO);
  assert(f.m.wifi_config_owned && !f.disconnects);
  wifi_available(&f);
  f.wifi_error = 0;
  f.connect_error = H2_PAL_ERR_IO;
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_IO);
  assert(f.m.wifi_config_owned && f.m.wifi_previous_valid);
  assert(f.disconnects == 1 && f.connects == 2);
  wifi_available(&f);
  f.connect_error = 0;
  cleanup(&f);
  assert(f.disconnects == 1 && f.connects == 3);
}

static void configured_wifi_failure_and_timeout(void) {
  fake_t f;
  setup(&f, H2_MFG_WIFI);
  f.profile.wifi_test_config = &test_network;
  f.profile.wifi_timeout_ms = 1000;
  f.connect_error = H2_PAL_ERR_IO;
  assert(h2_mfg_hardware_enter(&f.m, 100) == H2_PAL_ERR_IO);
  assert(f.m.wifi_config_owned && f.connects == 1);
  wifi_available(&f);
  assert(h2_mfg_hardware_leave(&f.m) == 0);
  f.connect_error = 0;
  assert(h2_mfg_hardware_enter(&f.m, 200) == 0);
  assert(tick(&f, 1199) == 0 && !f.m.model.ready);
  assert(tick(&f, 1200) == H2_PAL_ERR_TIMEOUT && !f.m.model.ready);
  f.disconnect_error = H2_PAL_ERR_BUSY;
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
  assert(f.m.wifi_config_owned);
  wifi_available(&f);
  f.disconnect_error = 0;
  assert(h2_mfg_hardware_leave(&f.m) == 0);
  assert(h2_mfg_hardware_enter(&f.m, 1300) == 0);
  f.status.state = H2_PAL_WIFI_STA_STATE_FAILED;
  assert(tick(&f, 1301) == H2_PAL_ERR_IO && !f.m.model.ready);
  cleanup(&f);
  assert(f.connects == 3 && f.disconnects == 4 && !f.saved_reads);
}

static void configured_wifi_accepts_only_inflight_target_timeout(void) {
  fake_t f;
  setup(&f, H2_MFG_WIFI);
  f.profile.wifi_test_config = &test_network;
  f.profile.wifi_timeout_ms = 15000;
  f.connect_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(f.connect_timeout == 1000 && !f.m.model.ready);
  f.connect_error = 0;
  cleanup(&f);
}

static void configured_wifi_respects_other_owners(void) {
  fake_t f;
  setup(&f, H2_MFG_WIFI);
  f.profile.wifi_test_config = &test_network;
  f.wifi_operation_locked = 1; /* Another task owns the Loader WiFi mutex. */
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_WOULD_BLOCK);
  assert(!f.connects && !f.m.wifi_lock_owned && !f.m.wifi_config_owned);
  f.wifi_operation_locked = 0;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  f.status.ssid[0] =
      'X'; /* A remote Loader command selected another network. */
  f.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  f.status.ip_valid = 1;
  f.status.ip.ip4 = 0xc0000202;
  assert(tick(&f, 20) == 0 && !f.m.model.ready);
  cleanup(&f);
  assert(!f.disconnects && f.connects == 1);

  setup(&f, H2_MFG_WIFI);
  f.profile.wifi_test_config = &test_network;
  f.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  memcpy(f.status.ssid, previous_network.ssid, previous_network.ssid_len);
  f.status.ssid_len = previous_network.ssid_len;
  f.saved_error = H2_PAL_ERR_NOT_FOUND;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_NOT_FOUND);
  assert(!f.connects && !f.m.wifi_config_owned);
  f.saved_error = 0;
  f.saved_config = test_network;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_BUSY);
  assert(!f.connects && !f.m.wifi_config_owned);
  cleanup(&f);
}

static void configured_wifi_observes_existing_test_link(void) {
  fake_t f;
  setup(&f, H2_MFG_WIFI);
  f.profile.wifi_test_config = &test_network;
  f.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  memcpy(f.status.ssid, test_network.ssid, test_network.ssid_len);
  f.status.ssid_len = test_network.ssid_len;
  f.status.ip_valid = 1;
  f.status.ip.ip4 = 0xc0000201;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(tick(&f, 20) == 0 && f.m.model.ready);
  cleanup(&f);
  assert(!f.connects && !f.disconnects && !f.saved_reads);
}

static void default_wifi_still_observes_loader(void) {
  fake_t f;
  setup(&f, H2_MFG_WIFI);
  f.profile.wifi_manual_ipv4 = 0;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(tick(&f, 60000) == 0 && !f.m.model.ready);
  f.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  f.status.ip.ip6_valid = 1;
  f.status.ip.ip6[0] = 0x20;
  f.status.ip.ip6[1] = 1;
  assert(tick(&f, 60020) == 0 && f.m.model.ready);
  cleanup(&f);
  assert(!f.connects && !f.disconnects && !f.saved_reads);
}

static void default_ble_still_requires_probe(void) {
  fake_t f;
  setup(&f, H2_MFG_BLE);
  f.profile.ble_scan_only = 0;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(!f.starts && !f.scans && !f.m.scan_mutex);
  assert(tick(&f, 0) == 0 && !f.m.model.ready);
  f.m.ble_probed = 1;
  assert(tick(&f, 20) == 0 && f.m.model.ready);
  cleanup(&f);
}

int main(void) {
  bounded_copied_scan_and_stop();
  scan_errors_keep_real_ownership();
  manual_wifi_requires_current_ipv4();
  configured_wifi_connects_and_restores();
  configured_wifi_restoration_retries();
  configured_wifi_failure_and_timeout();
  configured_wifi_respects_other_owners();
  configured_wifi_accepts_only_inflight_target_timeout();
  configured_wifi_observes_existing_test_link();
  default_wifi_still_observes_loader();
  default_ble_still_requires_probe();
  return 0;
}
