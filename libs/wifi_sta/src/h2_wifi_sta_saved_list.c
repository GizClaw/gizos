#include "h2_wifi_sta.h"

#include <string.h>

static uint32_t read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static void write_u32(uint8_t *p, uint32_t value) {
  for (size_t i = 0; i < 4u; ++i)
    p[i] = (uint8_t)(value >> (8u * i));
}

static int valid_config(const h2_pal_wifi_sta_config_t *config) {
  if (h2_pal_wifi_settings_validate_sta_config(config) != H2_PAL_OK ||
      config->bssid_set > 1u)
    return 0;
  uint8_t channel = config->channel;
  return channel <= 14u ||
         (channel >= 36u && channel <= 64u && channel % 4u == 0u) ||
         (channel >= 100u && channel <= 144u && channel % 4u == 0u) ||
         (channel >= 149u && channel <= 177u && (channel - 149u) % 4u == 0u);
}

static int same_ssid(const h2_pal_wifi_sta_config_t *config, const char *ssid,
                     size_t len) {
  return config->ssid_len == len && memcmp(config->ssid, ssid, len) == 0;
}

static int valid_list(const h2_pal_wifi_saved_network_t *saved, size_t count) {
  if (count > H2_PAL_WIFI_SAVED_NETWORK_MAX || (count && !saved))
    return 0;
  for (size_t i = 0; i < count; ++i) {
    if (!valid_config(&saved[i].config))
      return 0;
    for (size_t j = 0; j < i; ++j)
      if (same_ssid(&saved[j].config, saved[i].config.ssid,
                    saved[i].config.ssid_len))
        return 0;
  }
  return 1;
}

int h2_wifi_saved_list_decode(const uint8_t *blob, size_t blob_size,
                              h2_pal_wifi_saved_network_t *out,
                              size_t *out_count) {
  if (!out_count)
    return H2_PAL_ERR_INVALID_ARG;
  *out_count = 0;
  if (!blob || !out)
    return H2_PAL_ERR_INVALID_ARG;
  if (blob_size != H2_WIFI_SAVED_LIST_BLOB_SIZE ||
      read_u32(blob) != H2_WIFI_SAVED_LIST_MAGIC || blob[4] != 1u ||
      blob[5] != 0u || blob[6] > H2_PAL_WIFI_SAVED_NETWORK_MAX || blob[7] != 0u)
    return H2_PAL_ERR_FORMAT;
  h2_pal_wifi_saved_network_t decoded[H2_PAL_WIFI_SAVED_NETWORK_MAX] = {0};
  for (size_t i = 0; i < blob[6]; ++i) {
    const uint8_t *p = blob + 8u + i * H2_WIFI_SAVED_LIST_RECORD_SIZE;
    h2_pal_wifi_sta_config_t *c = &decoded[i].config;
    c->ssid_len = p[0];
    c->password_len = p[1];
    memcpy(c->bssid, p + 2u, 6u);
    c->bssid_set = p[8];
    c->channel = p[9];
    if (!valid_config(c))
      return H2_PAL_ERR_FORMAT;
    memcpy(c->ssid, p + 10u, c->ssid_len);
    memcpy(c->password, p + 42u, c->password_len);
    decoded[i].last_connected_seq = read_u32(p + 106u);
  }
  if (!valid_list(decoded, blob[6]))
    return H2_PAL_ERR_FORMAT;
  memcpy(out, decoded, sizeof(decoded));
  *out_count = blob[6];
  return H2_PAL_OK;
}

int h2_wifi_saved_list_encode(const h2_pal_wifi_saved_network_t *saved,
                              size_t count, uint8_t *blob, size_t blob_size) {
  if (!blob || blob_size != H2_WIFI_SAVED_LIST_BLOB_SIZE ||
      !valid_list(saved, count))
    return H2_PAL_ERR_INVALID_ARG;
  memset(blob, 0, blob_size);
  write_u32(blob, H2_WIFI_SAVED_LIST_MAGIC);
  blob[4] = 1u;
  blob[6] = (uint8_t)count;
  for (size_t i = 0; i < count; ++i) {
    uint8_t *p = blob + 8u + i * H2_WIFI_SAVED_LIST_RECORD_SIZE;
    const h2_pal_wifi_sta_config_t *c = &saved[i].config;
    p[0] = (uint8_t)c->ssid_len;
    p[1] = (uint8_t)c->password_len;
    memcpy(p + 2u, c->bssid, 6u);
    p[8] = c->bssid_set;
    p[9] = c->channel;
    memcpy(p + 10u, c->ssid, c->ssid_len);
    memcpy(p + 42u, c->password, c->password_len);
    write_u32(p + 106u, saved[i].last_connected_seq);
  }
  return H2_PAL_OK;
}

int h2_wifi_saved_list_insert(h2_pal_wifi_saved_network_t *saved, size_t *count,
                              const h2_pal_wifi_sta_config_t *config) {
  if (!saved || !count || !valid_config(config) || !valid_list(saved, *count))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_wifi_saved_network_t entry = {.config = *config};
  entry.config.ssid[config->ssid_len] = '\0';
  entry.config.password[config->password_len] = '\0';
  uint32_t seq = 0;
  size_t at = *count;
  for (size_t i = 0; i < *count; ++i) {
    if (same_ssid(&saved[i].config, config->ssid, config->ssid_len))
      at = i;
    if (saved[i].last_connected_seq > seq)
      seq = saved[i].last_connected_seq;
  }
  /* Renormalize before overflow, preserving the authoritative list order. */
  if (seq == UINT32_MAX) {
    for (size_t i = 0; i < *count; ++i)
      saved[i].last_connected_seq = (uint32_t)(*count - i);
    seq = (uint32_t)*count;
  }
  entry.last_connected_seq = seq + 1u;
  if (at == *count && *count < H2_PAL_WIFI_SAVED_NETWORK_MAX)
    ++*count;
  if (at >= *count)
    at = *count - 1u;
  memmove(saved + 1u, saved, at * sizeof(*saved));
  saved[0] = entry;
  return H2_PAL_OK;
}

int h2_wifi_saved_list_remove(h2_pal_wifi_saved_network_t *saved, size_t *count,
                              const char *ssid, size_t ssid_len) {
  if (!saved || !count || !ssid || !ssid_len ||
      ssid_len > H2_PAL_WIFI_SSID_MAX || !valid_list(saved, *count))
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < *count; ++i) {
    if (!same_ssid(&saved[i].config, ssid, ssid_len))
      continue;
    memmove(saved + i, saved + i + 1u, (*count - i - 1u) * sizeof(*saved));
    memset(saved + --*count, 0, sizeof(*saved));
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

int h2_wifi_sta_rank_saved_candidates(const h2_pal_wifi_saved_network_t *saved,
                                      size_t saved_count,
                                      const h2_pal_wifi_scan_entry_t *scan,
                                      size_t scan_count,
                                      h2_pal_wifi_sta_config_t *out,
                                      size_t *out_count) {
  if (!out_count)
    return H2_PAL_ERR_INVALID_ARG;
  *out_count = 0;
  if (!out || (scan_count && !scan) || !valid_list(saved, saved_count))
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < scan_count; ++i)
    if (scan[i].ssid_len > H2_PAL_WIFI_SSID_MAX)
      return H2_PAL_ERR_INVALID_ARG;
  int rssis[H2_PAL_WIFI_SAVED_NETWORK_MAX] = {0};
  uint32_t sequences[H2_PAL_WIFI_SAVED_NETWORK_MAX] = {0};
  for (size_t i = 0; i < saved_count; ++i) {
    const h2_pal_wifi_scan_entry_t *best = NULL;
    for (size_t j = 0; j < scan_count; ++j)
      if (same_ssid(&saved[i].config, scan[j].ssid, scan[j].ssid_len) &&
          (!best || scan[j].rssi > best->rssi))
        best = &scan[j];
    if (!best)
      continue;
    h2_pal_wifi_sta_config_t candidate = saved[i].config;
    memcpy(candidate.bssid, best->bssid, sizeof(candidate.bssid));
    candidate.bssid_set = 1u;
    candidate.channel = best->channel;
    if (!valid_config(&candidate))
      return H2_PAL_ERR_INVALID_ARG;
    size_t at = *out_count;
    while (at > 0u && (best->rssi > rssis[at - 1u] ||
                       (best->rssi == rssis[at - 1u] &&
                        saved[i].last_connected_seq > sequences[at - 1u]))) {
      out[at] = out[at - 1u];
      rssis[at] = rssis[at - 1u];
      sequences[at] = sequences[at - 1u];
      --at;
    }
    out[at] = candidate;
    rssis[at] = best->rssi;
    sequences[at] = saved[i].last_connected_seq;
    ++*out_count;
  }
  return H2_PAL_OK;
}
