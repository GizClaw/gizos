#include "h2_runtime_internal.h"

#include <string.h>

#define H2_RUNTIME_WIFI_ADDRESS_WAIT_MS 8000u

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

/* One atomic pref replacement; never persist ABI padding or terminators. */
#define WIFI_SAVED_NAMESPACE "h2runtime_wifi"
#define WIFI_SAVED_KEY "saved_v1"
#define WIFI_SAVED_RECORD_SIZE 114u
#define WIFI_SAVED_BLOB_SIZE (8u + H2_RUNTIME_WIFI_SAVED_MAX * WIFI_SAVED_RECORD_SIZE)

static bool saved_config_valid(const h2_pal_wifi_sta_config_t *config) {
    return config && config->ssid_len > 0 && config->ssid_len <= H2_PAL_WIFI_SSID_MAX &&
           config->password_len <= H2_PAL_WIFI_PASSWORD_MAX && config->bssid_set <= 1;
}

static bool saved_ssid_equal(const h2_pal_wifi_sta_config_t *a, const h2_pal_wifi_sta_config_t *b) {
    return a->ssid_len == b->ssid_len && memcmp(a->ssid, b->ssid, a->ssid_len) == 0;
}

/* A platform without Preference simply has no saved set: listing is empty and
 * storing is unsupported, never an argument error from the caller. The
 * canonical unsupported provider is not detectable by inspection, because it
 * installs a real vtable whose open returns UNSUPPORTED, so reads treat that
 * result as an empty set too. */
static bool saved_storage_available(const h2_runtime_t *runtime) {
    return runtime->pref != NULL && runtime->pref->vtable != NULL &&
           runtime->pref->vtable->open != NULL;
}

static bool saved_storage_absent(int rc) {
    return rc == H2_PAL_ERR_NOT_FOUND || rc == H2_PAL_ERR_UNSUPPORTED;
}

static bool saved_bytes_zero(const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; ++i)
        if (bytes[i])
            return false;
    return true;
}

static int saved_read(h2_runtime_t *runtime, h2_runtime_wifi_saved_network_t *saved,
                      size_t *count) {
    *count = 0;
    if (!saved_storage_available(runtime))
        return H2_PAL_OK;
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = h2_pal_pref_open(runtime->pref, WIFI_SAVED_NAMESPACE, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (saved_storage_absent(rc))
        return H2_PAL_OK;
    if (rc != H2_PAL_OK)
        return rc;
    void *data = NULL;
    size_t len = 0;
    rc = ns->get_blob ? ns->get_blob(ns, runtime->mem, WIFI_SAVED_KEY, &data, &len)
                      : H2_PAL_ERR_UNSUPPORTED;
    int close_rc = ns->close(ns);
    if (saved_storage_absent(rc))
        rc = H2_PAL_OK;
    else if (rc == H2_PAL_OK) {
        const uint8_t *blob = data;
        if (!blob || len != WIFI_SAVED_BLOB_SIZE || memcmp(blob, "H2WN\1\0", 6) ||
            blob[6] > H2_RUNTIME_WIFI_SAVED_MAX || blob[7]) {
            rc = H2_PAL_ERR_FORMAT;
        } else {
            for (size_t i = 0; i < blob[6]; ++i) {
                const uint8_t *record = blob + 8 + i * WIFI_SAVED_RECORD_SIZE;
                h2_runtime_wifi_saved_network_t *entry = &saved[i];
                memset(entry, 0, sizeof(*entry));
                entry->config.ssid_len = record[0];
                entry->config.password_len = record[1];
                memcpy(entry->config.bssid, record + 2, 6);
                entry->config.bssid_set = record[8];
                entry->config.channel = record[9];
                memcpy(entry->config.ssid, record + 10, 32);
                memcpy(entry->config.password, record + 42, 64);
                for (size_t j = 0; j < 8; ++j)
                    entry->last_connected_at_ms |= (uint64_t)record[106 + j] << (j * 8);
                /* Only the canonical encoding is accepted: padding past each
                 * length carries no meaning, so a non-zero byte there means the
                 * blob was not written by this codec. */
                if (!saved_config_valid(&entry->config) ||
                    !saved_bytes_zero(record + 10 + entry->config.ssid_len,
                                      32 - entry->config.ssid_len) ||
                    !saved_bytes_zero(record + 42 + entry->config.password_len,
                                      64 - entry->config.password_len)) {
                    rc = H2_PAL_ERR_FORMAT;
                    break;
                }
                for (size_t j = 0; j < i; ++j)
                    if (saved_ssid_equal(&saved[j].config, &entry->config))
                        rc = H2_PAL_ERR_FORMAT;
                if (rc != H2_PAL_OK)
                    break;
            }
            /* Slots past the count are unused and must be zero as written. */
            if (rc == H2_PAL_OK &&
                !saved_bytes_zero(blob + 8 + blob[6] * WIFI_SAVED_RECORD_SIZE,
                                  (H2_RUNTIME_WIFI_SAVED_MAX - blob[6]) * WIFI_SAVED_RECORD_SIZE))
                rc = H2_PAL_ERR_FORMAT;
            if (rc == H2_PAL_OK)
                *count = blob[6];
        }
    }
    if (data) {
        memset(data, 0, len);
        h2_pal_mem_free(runtime->mem, data);
    }
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int saved_write(h2_runtime_t *runtime, const h2_runtime_wifi_saved_network_t *saved,
                       size_t count) {
    uint8_t blob[WIFI_SAVED_BLOB_SIZE] = {0};
    memcpy(blob, "H2WN\1\0", 6);
    blob[6] = (uint8_t)count;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *record = blob + 8 + i * WIFI_SAVED_RECORD_SIZE;
        const h2_pal_wifi_sta_config_t *config = &saved[i].config;
        record[0] = (uint8_t)config->ssid_len;
        record[1] = (uint8_t)config->password_len;
        memcpy(record + 2, config->bssid, 6);
        record[8] = config->bssid_set;
        record[9] = config->channel;
        memcpy(record + 10, config->ssid, config->ssid_len);
        memcpy(record + 42, config->password, config->password_len);
        for (size_t j = 0; j < 8; ++j)
            record[106 + j] = (uint8_t)(saved[i].last_connected_at_ms >> (j * 8));
    }
    if (!saved_storage_available(runtime)) {
        memset(blob, 0, sizeof(blob));
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_pref_namespace_t *ns = NULL;
    int rc =
        h2_pal_pref_open(runtime->pref, WIFI_SAVED_NAMESPACE, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc == H2_PAL_OK) {
        rc = ns->set_blob ? ns->set_blob(ns, WIFI_SAVED_KEY, blob, sizeof(blob))
                          : H2_PAL_ERR_UNSUPPORTED;
        /* Providers that buffer writes discard them on close without this. */
        if (rc == H2_PAL_OK)
            rc = ns->commit ? ns->commit(ns) : H2_PAL_ERR_UNSUPPORTED;
        int close_rc = ns->close(ns);
        if (rc == H2_PAL_OK)
            rc = close_rc;
    }
    memset(blob, 0, sizeof(blob));
    return rc;
}

h2_pal_result_t h2_runtime_wifi_saved_list(h2_runtime_t *runtime,
                                           h2_runtime_wifi_saved_network_t *out, size_t capacity,
                                           size_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (!h2_runtime_ready(runtime) || !out_count || (capacity && !out))
        return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_wifi_saved_network_t saved[H2_RUNTIME_WIFI_SAVED_MAX] = {0};
    size_t count = 0;
    int rc = saved_read(runtime, saved, &count);
    if (rc == H2_PAL_OK) {
        *out_count = count < capacity ? count : capacity;
        if (*out_count)
            memcpy(out, saved, *out_count * sizeof(*out));
    }
    memset(saved, 0, sizeof(saved));
    return rc;
}

h2_pal_result_t h2_runtime_wifi_saved_save(h2_runtime_t *runtime,
                                           const h2_pal_wifi_sta_config_t *config) {
    if (!h2_runtime_ready(runtime) || !saved_config_valid(config))
        return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_wifi_saved_network_t saved[H2_RUNTIME_WIFI_SAVED_MAX] = {0};
    size_t count = 0;
    int rc = saved_read(runtime, saved, &count);
    if (rc == H2_PAL_OK) {
        size_t position = 0;
        while (position < count && !saved_ssid_equal(&saved[position].config, config))
            ++position;
        if (position == count && count < H2_RUNTIME_WIFI_SAVED_MAX)
            ++count;
        if (position == H2_RUNTIME_WIFI_SAVED_MAX)
            --position;
        memmove(saved + 1, saved, position * sizeof(*saved));
        saved[0] = (h2_runtime_wifi_saved_network_t){.config = *config};
        (void)h2_pal_time_get_wall_ms(runtime->time, &saved[0].last_connected_at_ms);
        rc = saved_write(runtime, saved, count);
    }
    memset(saved, 0, sizeof(saved));
    return rc;
}

h2_pal_result_t h2_runtime_wifi_saved_remove(h2_runtime_t *runtime, const char *ssid,
                                             size_t ssid_len) {
    if (!h2_runtime_ready(runtime) || !ssid || !ssid_len || ssid_len > H2_PAL_WIFI_SSID_MAX)
        return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_wifi_saved_network_t saved[H2_RUNTIME_WIFI_SAVED_MAX] = {0};
    size_t count = 0;
    int rc = saved_read(runtime, saved, &count);
    if (rc == H2_PAL_OK) {
        rc = H2_PAL_ERR_NOT_FOUND;
        for (size_t i = 0; i < count; ++i) {
            if (saved[i].config.ssid_len == ssid_len &&
                memcmp(saved[i].config.ssid, ssid, ssid_len) == 0) {
                memmove(saved + i, saved + i + 1, (count - i - 1) * sizeof(*saved));
                rc = saved_write(runtime, saved, count - 1);
                break;
            }
        }
    }
    memset(saved, 0, sizeof(saved));
    return rc;
}

h2_pal_result_t h2_runtime_wifi_saved_clear(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime))
        return H2_PAL_ERR_INVALID_ARG;
    return saved_write(runtime, NULL, 0);
}

h2_pal_result_t h2_runtime_wifi_connect_and_save(h2_runtime_t *runtime,
                                                 const h2_pal_wifi_sta_config_t *config,
                                                 uint32_t timeout_ms) {
    if (!h2_runtime_ready(runtime) || !saved_config_valid(config))
        return H2_PAL_ERR_INVALID_ARG;
    int rc = h2_pal_wifi_sta_connect_and_save(runtime->wifi_sta, config, timeout_ms);
    if (rc != H2_PAL_OK)
        return rc;
    /* The station is connected and the platform credential is stored. Recording
     * it in the Runtime set is bookkeeping: report the failure, but never turn a
     * working connection into a provisioning failure. */
    const int save_rc = h2_runtime_wifi_saved_save(runtime, config);
    if (save_rc != H2_PAL_OK)
        (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_WARN, "runtime/wifi",
                               "saved set not updated after provisioning");
    return H2_PAL_OK;
}

/* Retain only the strongest AP for each saved SSID, even for long scans. */
typedef struct saved_scan {
    h2_runtime_wifi_saved_network_t saved[H2_RUNTIME_WIFI_SAVED_MAX];
    size_t saved_count;
    h2_pal_wifi_scan_entry_t visible[H2_RUNTIME_WIFI_SAVED_MAX];
    size_t visible_count;
} saved_scan_t;

static bool collect_saved_ap(void *user, const h2_pal_wifi_scan_entry_t *entry) {
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

static int wifi_remaining(h2_runtime_t *runtime, uint64_t started, uint32_t budget,
                          uint32_t *remaining) {
    uint64_t now = 0;
    int rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK)
        return rc;
    if (now < started || now - started >= budget)
        return H2_PAL_ERR_TIMEOUT;
    *remaining = budget - (uint32_t)(now - started);
    return H2_PAL_OK;
}

/* The generation is captured before connect, including synchronous PAL events.
 * Never query PAL status here: only the existing event handler publishes state.
 */
static int wifi_wait_for_address(h2_runtime_t *runtime,
                                 const h2_pal_wifi_sta_config_t *candidate,
                                 uint32_t generation, uint64_t started,
                                 uint32_t budget) {
  struct h2_runtime_private *state = runtime->private_state;
  h2_runtime_system_wifi_sta_state_t station = {0};
  if (!state->wifi_connect_wait.cond) {
    (void)h2_runtime_system_state_wifi_sta(runtime, &station);
    return H2_PAL_OK;
  }
  uint64_t wait_started = 0;
  int rc = h2_pal_time_get_monotonic_ms(runtime->time, &wait_started);
  if (rc != H2_PAL_OK)
    return rc;
  if (wait_started < started || wait_started - started >= budget)
    return H2_PAL_ERR_UNAVAILABLE;
  uint32_t window = budget - (uint32_t)(wait_started - started);
  if (window > H2_RUNTIME_WIFI_ADDRESS_WAIT_MS)
    window = H2_RUNTIME_WIFI_ADDRESS_WAIT_MS;
  int wait_rc = H2_PAL_OK;
  /* Without the lock the generation and the condition cannot be read safely,
   * so the candidate is abandoned rather than accepted on association alone. */
  if (h2_pal_mutex_lock(runtime->sync, state->wifi_connect_wait.mutex) != H2_PAL_OK)
    return H2_PAL_ERR_UNAVAILABLE;
  for (;;) {
    if (generation != state->wifi_connect_wait.generation) {
      generation = state->wifi_connect_wait.generation;
      rc = h2_runtime_system_state_wifi_sta(runtime, &station);
      if (rc != H2_PAL_OK)
        break;
      if (station.valid) {
        /* A zero IPv4 is not an address, whatever the valid flag says. */
        if (station.status == H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_GOT_IP &&
            station.ip_valid && station.ip.ip4 != 0u &&
            station.ssid_len == candidate->ssid_len &&
            !memcmp(station.ssid, candidate->ssid, candidate->ssid_len))
          break;
        /* Losing the address leaves the snapshot without a valid one, so the
         * candidate simply runs out its window; no event kind is retained. */
        if (station.status == H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_DISCONNECTED ||
            station.status == H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_FAILED) {
          rc = H2_PAL_ERR_UNAVAILABLE;
          break;
        }
      }
    }
    if (wait_rc != H2_PAL_OK) {
      rc = wait_rc == H2_PAL_ERR_TIMEOUT ? H2_PAL_ERR_UNAVAILABLE : wait_rc;
      break;
    }
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK)
      break;
    if (now < wait_started || now - wait_started >= window) {
      rc = H2_PAL_ERR_UNAVAILABLE;
      break;
    }
    uint32_t remaining = window - (uint32_t)(now - wait_started);
    wait_rc = h2_pal_cond_wait(runtime->sync, state->wifi_connect_wait.cond,
                               state->wifi_connect_wait.mutex, remaining);
  }
  h2_pal_mutex_unlock(runtime->sync, state->wifi_connect_wait.mutex);
  return rc;
}

h2_pal_result_t h2_runtime_wifi_connect_best_saved(h2_runtime_t *runtime, uint32_t timeout_ms) {
    if (!h2_runtime_ready(runtime))
        return H2_PAL_ERR_INVALID_ARG;
    uint32_t budget = timeout_ms ? timeout_ms : 15000u;
    uint32_t remaining = budget;
    uint64_t started = 0;
    int rc = h2_pal_time_get_monotonic_ms(runtime->time, &started);
    if (rc != H2_PAL_OK)
        return rc;
    saved_scan_t scan = {0};
    h2_pal_wifi_sta_config_t candidates[H2_RUNTIME_WIFI_SAVED_MAX] = {0};
    /* Candidates are pinned to the AP just observed; the stored entry keeps the
     * credential as provisioned, so a later attempt is not tied to that AP. */
    h2_pal_wifi_sta_config_t origins[H2_RUNTIME_WIFI_SAVED_MAX] = {0};
    size_t count = 0;
    rc = h2_runtime_wifi_saved_list(runtime, scan.saved, H2_RUNTIME_WIFI_SAVED_MAX,
                                    &scan.saved_count);
    if (rc != H2_PAL_OK)
        goto done;
    if (scan.saved_count > H2_RUNTIME_WIFI_SAVED_MAX) {
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
    rc = h2_pal_wifi_sta_scan(runtime->wifi_sta, NULL, collect_saved_ap, &scan, remaining);
    if (rc != H2_PAL_OK)
        goto done;
    int strengths[H2_RUNTIME_WIFI_SAVED_MAX] = {0};
    /* Stable insertion preserves stored recency for equal RSSI. */
    for (size_t i = 0; i < scan.saved_count; ++i) {
        for (size_t j = 0; j < scan.visible_count; ++j) {
            const h2_pal_wifi_scan_entry_t *ap = &scan.visible[j];
            h2_pal_wifi_sta_config_t candidate = scan.saved[i].config;
            if (candidate.ssid_len != ap->ssid_len ||
                memcmp(candidate.ssid, ap->ssid, ap->ssid_len))
                continue;
            memcpy(candidate.bssid, ap->bssid, sizeof(candidate.bssid));
            candidate.bssid_set = 1;
            candidate.channel = ap->channel;
            size_t position = count;
            while (position && strengths[position - 1] < ap->rssi) {
                strengths[position] = strengths[position - 1];
                candidates[position] = candidates[position - 1];
                origins[position] = origins[position - 1];
                --position;
            }
            strengths[position] = ap->rssi;
            candidates[position] = candidate;
            origins[position] = scan.saved[i].config;
            memset(&candidate, 0, sizeof(candidate));
            ++count;
            break;
        }
    }
    rc = H2_PAL_ERR_NOT_FOUND;
    for (size_t i = 0; i < count; ++i) {
        int budget_rc = wifi_remaining(runtime, started, budget, &remaining);
        if (budget_rc != H2_PAL_OK) {
            rc = budget_rc;
            break;
        }
        uint32_t generation = 0;
        if (runtime->private_state->wifi_connect_wait.cond != NULL) {
          /* Without a generation captured before connect, a station event that
           * already happened could be mistaken for this attempt's result, so
           * the candidate is abandoned before it starts. */
          if (h2_pal_mutex_lock(
                  runtime->sync,
                  runtime->private_state->wifi_connect_wait.mutex) !=
              H2_PAL_OK) {
            rc = H2_PAL_ERR_UNAVAILABLE;
            continue;
          }
          generation = runtime->private_state->wifi_connect_wait.generation;
          (void)h2_pal_mutex_unlock(
              runtime->sync, runtime->private_state->wifi_connect_wait.mutex);
        }
        rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &candidates[i], remaining);
        if (rc == H2_PAL_OK)
          rc = wifi_wait_for_address(runtime, &candidates[i], generation,
                                     started, budget);
        if (rc == H2_PAL_OK) {
            /* Fronting the entry is bookkeeping: report it, but never turn a
             * working connection into a failure. */
            if (h2_runtime_wifi_saved_save(runtime, &origins[i]) != H2_PAL_OK)
                (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_WARN, "runtime/wifi",
                                       "saved set not reordered after reconnect");
            break;
        }
    }
done:
    memset(candidates, 0, sizeof(candidates));
    memset(origins, 0, sizeof(origins));
    memset(&scan, 0, sizeof(scan));
    return rc;
}
