#include "h2_web_main_thread.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdio.h>
#include <string.h>

#define CHECK(test)                                                            \
  do {                                                                         \
    if (!(test)) {                                                             \
      fprintf(stderr, "fake network line %d: %s\n", __LINE__, #test);          \
      emscripten_force_exit(1);                                                \
    }                                                                          \
  } while (0)

/* clang-format off */
EM_JS(void, configure, (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32'], null, mode => {
    globalThis.h2TestNetif.set(mode !== 6);
    if (mode === 0) { delete Module.h2WebEnvironment; return; }
    Module.h2WebEnvironment = {version: 1,
      wifi: {enabled: mode === 1 || mode === 6, connected: true, ssid: 'Browser Wi-Fi', rssi: -48},
      modem: {enabled: mode !== 3, simPresent: mode !== 4, registered: true, rssi: -70}};
    if (mode === 5) Module.h2WebEnvironment.wifi.rssi = 99;
  });
});
/* clang-format on */

static void set_mode(int mode) {
  (void)h2_web_main_call(configure, (const void *[]){&mode});
}
static h2_pal_result_t destroy_idle(h2_web_platform_t *platform) {
  h2_pal_result_t rc = H2_PAL_ERR_BUSY;
  for (unsigned i = 0; i < 1000 && rc == H2_PAL_ERR_BUSY; ++i) {
    rc = h2_web_platform_destroy(platform);
    if (rc == H2_PAL_ERR_BUSY)
      emscripten_thread_sleep(1);
  }
  return rc;
}

static bool scan(void *user, const h2_pal_wifi_scan_entry_t *entry) {
  ++*(int *)user;
  CHECK(strcmp(entry->ssid, "Browser Wi-Fi") == 0);
  CHECK(entry->security == H2_PAL_WIFI_SECURITY_OPEN);
  return false;
}

int main(void) {
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  set_mode(0);
  h2_web_platform_t *p = h2_web_platform_create(&config);
  CHECK(p != NULL);
  CHECK(h2_web_platform_fake_wifi_sta_api(p) == NULL);
  CHECK(h2_web_platform_fake_modem_api(p) == NULL);
  CHECK(destroy_idle(p) == H2_PAL_OK);
  set_mode(5);
  CHECK(h2_web_platform_create(&config) == NULL);
  set_mode(1);
  p = h2_web_platform_create(&config);
  CHECK(p != NULL);
  const h2_pal_wifi_sta_api_t *wifi = h2_web_platform_fake_wifi_sta_api(p);
  const h2_pal_wifi_settings_api_t *settings =
      h2_web_platform_fake_wifi_settings_api(p);
  const h2_pal_modem_api_t *modem = h2_web_platform_fake_modem_api(p);
  CHECK(wifi && settings && modem);
  h2_pal_wifi_sta_status_t status;
  CHECK(h2_pal_wifi_sta_get_status(wifi, &status) == H2_PAL_OK);
  CHECK(status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.rssi == -48 &&
        status.ip_valid);
  int scanned = 0;
  CHECK(h2_pal_wifi_sta_scan(wifi, NULL, scan, &scanned, 10) == H2_PAL_OK &&
        scanned == 1);
  h2_pal_wifi_sta_config_t saved;
  CHECK(h2_pal_wifi_settings_get_saved_sta_config(settings, &saved) ==
        H2_PAL_OK);
  CHECK(h2_pal_wifi_sta_disconnect(wifi) == H2_PAL_OK);
  CHECK(h2_pal_wifi_sta_get_status(wifi, &status) == H2_PAL_OK &&
        !status.ip_valid);
  CHECK(h2_pal_wifi_settings_clear_saved_sta_config(settings) == H2_PAL_OK);
  CHECK(h2_pal_wifi_sta_connect(wifi, &saved, 10) == H2_PAL_OK);
  int has = -1;
  CHECK(h2_pal_wifi_settings_has_saved_sta_config(settings, &has) ==
            H2_PAL_OK &&
        !has);
  CHECK(h2_pal_wifi_sta_connect_and_save(wifi, &saved, 10) == H2_PAL_OK);
  CHECK(h2_pal_wifi_settings_has_saved_sta_config(settings, &has) ==
            H2_PAL_OK &&
        has);
  saved.ssid[0] = '!';
  CHECK(h2_pal_wifi_sta_connect_and_save(wifi, &saved, 10) ==
        H2_PAL_ERR_UNAVAILABLE);
  CHECK(h2_pal_wifi_settings_get_saved_sta_config(settings, &saved) ==
            H2_PAL_OK &&
        saved.ssid[0] == 'B');

  CHECK(h2_pal_modem_open(modem, 10) == H2_PAL_OK);
  CHECK(h2_pal_modem_data_open(modem, 10) == H2_PAL_OK);
  h2_pal_modem_data_status_t data;
  CHECK(h2_pal_modem_get_data_status(modem, &data) == H2_PAL_OK &&
        data.state == H2_PAL_MODEM_DATA_OPEN);
  h2_pal_modem_signal_t signal;
  CHECK(h2_pal_modem_get_signal(modem, &signal) == H2_PAL_OK &&
        signal.rssi_valid && signal.rssi_dbm == -70);
  h2_pal_modem_identity_t identity;
  CHECK(h2_pal_modem_get_identity(modem, &identity) == H2_PAL_OK &&
        !identity.imei[0] && !identity.imsi[0]);
  h2_pal_modem_call_request_t call = {.number = "123", .timeout_ms = 1};
  CHECK(h2_pal_modem_call_dial(modem, &call) == H2_PAL_ERR_UNSUPPORTED);
  set_mode(2);
  CHECK(h2_pal_wifi_sta_get_status(wifi, &status) == H2_PAL_OK &&
        !status.ip_valid);
  const h2_pal_netif_api_t *netif = h2_web_platform_netif_api(p);
  const h2_pal_netif_ref_t ref = h2_pal_netif_default_ref();
  h2_pal_netif_status_t net;
  CHECK(h2_pal_netif_get_status(netif, &ref, &net) == H2_PAL_OK);
  CHECK(h2_pal_netif_status_is_usable(&net));
  set_mode(6);
  CHECK(h2_pal_wifi_sta_get_status(wifi, &status) == H2_PAL_OK &&
        !status.ip_valid);
  CHECK(h2_pal_modem_get_data_status(modem, &data) == H2_PAL_OK &&
        data.state == H2_PAL_MODEM_DATA_CLOSED);
  CHECK(h2_pal_netif_get_status(netif, &ref, &net) == H2_PAL_ERR_NOT_FOUND);
  set_mode(4);
  h2_pal_modem_status_t modem_state;
  CHECK(h2_pal_modem_get_status(modem, &modem_state) == H2_PAL_OK &&
        modem_state.sim == H2_PAL_MODEM_SIM_STATE_ABSENT);
  CHECK(h2_pal_modem_data_open(modem, 10) == H2_PAL_ERR_UNAVAILABLE);
  CHECK(h2_pal_modem_get_data_status(modem, &data) == H2_PAL_OK &&
        data.state == H2_PAL_MODEM_DATA_CLOSED);
  CHECK(h2_pal_netif_get_status(netif, &ref, &net) == H2_PAL_ERR_NOT_FOUND);
  set_mode(3);
  const char url[] = "https://example.invalid/offline";
  h2_pal_http_request_t request = {.url = {url, sizeof(url) - 1},
                                   .method = H2_PAL_HTTP_METHOD_GET};
  h2_pal_http_response_t response = {0};
  CHECK(h2_pal_http_request(h2_web_platform_http_api(p), &request, &response) ==
        H2_PAL_ERR_UNAVAILABLE);
  set_mode(2);
  CHECK(h2_pal_modem_data_close(modem, 10) == H2_PAL_OK);
  CHECK(h2_pal_modem_set_power_policy(
            modem, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) == H2_PAL_OK);
  h2_pal_modem_power_status_t power;
  CHECK(h2_pal_modem_get_power_status(modem, &power) == H2_PAL_OK &&
        power.state == H2_PAL_MODEM_POWER_STATE_ASLEEP);
  CHECK(h2_pal_modem_close(modem, 10) == H2_PAL_OK);
  CHECK(destroy_idle(p) == H2_PAL_OK);
  puts("H2_WEB_FAKE_NETWORK PASS");
  emscripten_force_exit(0);
  return 0;
}
