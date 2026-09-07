#ifndef H2_APP_TEST_WIFI_H
#define H2_APP_TEST_WIFI_H
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2_app_test_fault.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Scripted station and saved settings. scan entries/status are test inputs.
 * connect records the request but does not invent a GOT_IP completion; update
 * status and inject the corresponding Runtime event explicitly. Disconnect
 * success clears status to IDLE. No passwords are logged by this provider. */
typedef struct h2_app_test_wifi {
  h2_pal_wifi_sta_api_t api;
  h2_pal_wifi_settings_api_t settings;
  h2_pal_wifi_sta_status_t status;
  h2_pal_wifi_scan_entry_t entries[H2_PAL_WIFI_SCAN_MAX_RESULTS];
  size_t entry_count;
  bool saved_present;
  h2_pal_wifi_sta_config_t saved, last_connect;
  h2_pal_wifi_scan_request_t last_scan;
  uint32_t last_timeout_ms;
  h2_app_test_fault_t get_status, scan, connect, disconnect, get_saved,
      set_saved, clear_saved;
} h2_app_test_wifi_t;
/** Initialize caller-owned storage, default status IDLE and no saved network.
 */
void h2_app_test_wifi_init(h2_app_test_wifi_t *wifi);

#ifdef __cplusplus
}
#endif
#endif
