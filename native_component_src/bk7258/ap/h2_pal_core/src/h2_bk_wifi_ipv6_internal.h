#ifndef H2_BK_WIFI_IPV6_AP_H
#define H2_BK_WIFI_IPV6_AP_H

#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2_bk_wifi_ipv6.h"
#include <string.h>

/* Validate the fixed snapshot before any AP lwIP address change. */
static inline int
h2_bk_wifi_ipv6_snapshot_matches(const h2_bk_wifi_ipv6_snapshot_t *snapshot,
                                 const h2_pal_wifi_sta_config_t *config) {
  if (snapshot == NULL || config == NULL ||
      config->ssid_len > H2_PAL_WIFI_SSID_MAX)
    return 0;
  if (snapshot->version != H2_BK_WIFI_IPV6_VERSION ||
      snapshot->size != sizeof(*snapshot) || !snapshot->generation ||
      snapshot->connected > 1u || snapshot->count > H2_BK_WIFI_IPV6_MAX ||
      snapshot->ssid_len > H2_PAL_WIFI_SSID_MAX || snapshot->reserved[0] ||
      snapshot->reserved[1])
    return 0;
  if (!snapshot->connected)
    return snapshot->count == 0u;
  if (snapshot->ssid_len != config->ssid_len ||
      memcmp(snapshot->ssid, config->ssid, config->ssid_len) ||
      (config->bssid_set && memcmp(snapshot->bssid, config->bssid, 6u)))
    return 0;
  for (uint32_t i = 0u; i < snapshot->count; ++i)
    if (snapshot->addresses[i].preferred != 1u)
      return 0;
  return 1;
}

#endif
