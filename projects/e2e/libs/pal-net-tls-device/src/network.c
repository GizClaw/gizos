#include "h2_pal_net_tls_device.h"
#include <string.h>

int h2_net_tls_device_prepare_network(h2_runtime_t *runtime) {
  if (runtime == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_wifi_sta_status_t current = {0};
  if (h2_pal_wifi_sta_get_status(runtime->wifi_sta, &current) == H2_PAL_OK &&
      current.state == H2_PAL_WIFI_STA_STATE_GOT_IP && current.ip_valid &&
      current.ip.ip4 != 0u)
    return H2_PAL_OK;
  h2_pal_wifi_sta_config_t wifi = {0};
  int rc =
      h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &wifi);
  if (rc == H2_PAL_OK)
    rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &wifi, 20000u);
  memset(&wifi, 0, sizeof(wifi));
  if (rc != H2_PAL_OK)
    return rc;
  uint64_t start = 0u;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
  while (rc == H2_PAL_OK) {
    h2_pal_wifi_sta_status_t status = {0};
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (rc != H2_PAL_OK)
      return rc;
    if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
        status.ip.ip4 != 0u)
      return H2_PAL_OK;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (now - start >= 20000u)
      return H2_PAL_ERR_TIMEOUT;
    if (rc == H2_PAL_OK)
      rc = h2_pal_time_sleep_ms(runtime->time, 100u);
  }
  return rc;
}
