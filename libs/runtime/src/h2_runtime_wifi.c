#include "h2_runtime_internal.h"
#include "h2_wifi_sta.h"

#include <string.h>

h2_pal_result_t h2_runtime_wifi_connect_saved(h2_runtime_t *runtime,
                                             uint32_t timeout_ms) {
    if (!h2_runtime_ready(runtime))
        return H2_PAL_ERR_INVALID_ARG;
    h2_pal_wifi_sta_config_t config = {0};
    int rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &config);
    if (rc == H2_PAL_OK)
        rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &config, timeout_ms ? timeout_ms : 15000u);
    memset(&config, 0, sizeof(config));
    return rc;
}

/* Retain only the strongest AP for each saved SSID, even for long scans. */
typedef struct saved_scan {
  h2_pal_wifi_saved_network_t saved[H2_PAL_WIFI_SAVED_NETWORK_MAX];
  size_t saved_count;
  h2_pal_wifi_scan_entry_t visible[H2_PAL_WIFI_SAVED_NETWORK_MAX];
  size_t visible_count;
} saved_scan_t;

static bool collect_saved_ap(void *user,
                             const h2_pal_wifi_scan_entry_t *entry) {
  saved_scan_t *scan = user;
  if (!entry || entry->ssid_len > H2_PAL_WIFI_SSID_MAX)
    return true;
  for (size_t i = 0; i < scan->saved_count; ++i) {
    const h2_pal_wifi_sta_config_t *config = &scan->saved[i].config;
    if (config->ssid_len != entry->ssid_len ||
        memcmp(config->ssid, entry->ssid, entry->ssid_len) != 0)
      continue;
    for (size_t j = 0; j < scan->visible_count; ++j) {
      if (scan->visible[j].ssid_len == entry->ssid_len &&
          memcmp(scan->visible[j].ssid, entry->ssid, entry->ssid_len) == 0) {
        if (entry->rssi > scan->visible[j].rssi)
          scan->visible[j] = *entry;
        return true;
      }
    }
    scan->visible[scan->visible_count++] = *entry;
    break;
  }
  return true;
}

static int wifi_remaining(h2_runtime_t *runtime, uint64_t started,
                          uint32_t budget, uint32_t *remaining) {
  uint64_t now = 0;
  int rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (now < started || now - started >= budget)
    return H2_PAL_ERR_TIMEOUT;
  *remaining = budget - (uint32_t)(now - started);
  return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_wifi_connect_best_saved(h2_runtime_t *runtime,
                                                   uint32_t timeout_ms) {
  if (!h2_runtime_ready(runtime))
    return H2_PAL_ERR_INVALID_ARG;
  uint32_t budget = timeout_ms ? timeout_ms : 15000u;
  uint32_t remaining = budget;
  uint64_t started = 0;
  int rc = h2_pal_time_get_monotonic_ms(runtime->time, &started);
  if (rc != H2_PAL_OK)
    return rc;
  saved_scan_t scan = {0};
  h2_pal_wifi_sta_config_t candidates[H2_PAL_WIFI_SAVED_NETWORK_MAX] = {0};
  size_t count = 0;
  rc = h2_pal_wifi_settings_list_saved_sta_configs(
      runtime->wifi_settings, scan.saved, H2_PAL_WIFI_SAVED_NETWORK_MAX,
      &scan.saved_count);
  if (rc != H2_PAL_OK)
    goto done;
  if (scan.saved_count > H2_PAL_WIFI_SAVED_NETWORK_MAX) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  if (!scan.saved_count) {
    rc = H2_PAL_ERR_NOT_FOUND;
    goto done;
  }
  rc = wifi_remaining(runtime, started, budget, &remaining);
  if (rc != H2_PAL_OK)
    goto done;
  rc = h2_pal_wifi_sta_scan(runtime->wifi_sta, NULL, collect_saved_ap, &scan,
                            remaining);
  if (rc != H2_PAL_OK)
    goto done;
  rc = h2_wifi_sta_rank_saved_candidates(scan.saved, scan.saved_count,
                                         scan.visible, scan.visible_count,
                                         candidates, &count);
  if (rc != H2_PAL_OK)
    goto done;
  rc = H2_PAL_ERR_NOT_FOUND;
  for (size_t i = 0; i < count; ++i) {
    int budget_rc = wifi_remaining(runtime, started, budget, &remaining);
    if (budget_rc != H2_PAL_OK) {
      rc = budget_rc;
      break;
    }
    rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &candidates[i], remaining);
    if (rc == H2_PAL_OK)
      break;
  }
done:
  memset(candidates, 0, sizeof(candidates));
  memset(&scan, 0, sizeof(scan));
  return rc;
}
