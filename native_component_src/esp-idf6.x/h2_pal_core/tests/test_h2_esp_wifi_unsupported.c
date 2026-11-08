#include "h2_esp_platform_core.h"
#include "h2_pal.h"

#include <assert.h>

int main(void) {
  const h2_pal_wifi_ap_api_t* absent = h2_pal_unsupported_wifi_ap_api();
  const h2_pal_wifi_ap_api_t* native = h2_esp_platform_wifi_ap();
  h2_pal_wifi_ap_status_t status = {0};
  const h2_pal_wifi_ap_config_t config = {
      .ssid = "absent", .ssid_len = 6u, .security = H2_PAL_WIFI_SECURITY_OPEN};
  assert(h2_pal_wifi_ap_start(native, &config, 0u) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_pal_wifi_ap_stop(native, 0u) == h2_pal_wifi_ap_stop(absent, 0u));
  assert(h2_pal_wifi_ap_get_status(native, &status) ==
         h2_pal_wifi_ap_get_status(absent, &status));
  assert(h2_pal_wifi_ap_get_status(native, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_esp_platform_wifi_ensure_started() == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_esp_platform_wifi_connect_saved(0u) == H2_PAL_ERR_UNSUPPORTED);
  return 0;
}
