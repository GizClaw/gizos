#include "h2_gizclaw_e2e_internal.h"
#include <stdio.h>
#include <string.h>

#define KEY_WAIT_MS 30000u
#define PEER_WAIT_MS 90000u

static int elapsed(h2_gizclaw_e2e_fixture_t* f, uint64_t start,
                   uint32_t limit) {
  uint64_t now = 0u;
  int rc = h2_pal_time_get_monotonic_ms(f->time, &now);
  return rc                                    ? rc
         : now < start || now - start >= limit ? H2_PAL_ERR_TIMEOUT
                                               : H2_PAL_OK;
}
static int poll_service(h2_gizclaw_e2e_fixture_t* f) {
  size_t count = 0u;
  int rc = h2_gizclaw_service_poll(f->actors[0].service, 8u, &count);
  return rc ? rc : h2_pal_time_sleep_ms(f->time, 10u);
}
static int key_idle(h2_gizclaw_e2e_fixture_t* f,
                    h2_gizclaw_api_key_snapshot_t* out) {
  uint64_t start = 0u;
  int rc = h2_pal_time_get_monotonic_ms(f->time, &start);
  while (!rc) {
    rc = poll_service(f);
    if (!rc) rc = h2_gizclaw_api_key_state_snapshot(f->device_ble_key, out);
    if (rc || !out->busy) break;
    rc = elapsed(f, start, KEY_WAIT_MS);
  }
  return rc;
}
int h2_gizclaw_e2e_ble_binding_cleanup(h2_gizclaw_e2e_fixture_t* f) {
  int rc = H2_PAL_OK;
  if (f->device_ble_binding) {
    rc = h2_gizclaw_ble_binding_stop(f->device_ble_binding);
    h2_gizclaw_ble_binding_exposure_t exposure;
    while (!rc) {
      int next = h2_gizclaw_ble_binding_next_exposure(f->device_ble_binding,
                                                      &exposure);
      if (next == H2_PAL_ERR_WOULD_BLOCK) break;
      if (next) rc = next;
    }
    if (!rc) rc = h2_gizclaw_ble_binding_close(&f->device_ble_binding);
    if (rc) return rc;
  }
  if (f->device_ble_key) {
    h2_gizclaw_api_key_snapshot_t key = {0};
    rc = key_idle(f, &key);
    if (!rc && key.valid) {
      rc = h2_gizclaw_api_key_state_request_revoke(f->device_ble_key);
      if (!rc) rc = key_idle(f, &key);
      if (!rc && (key.valid || key.busy || key.last_error))
        rc = H2_PAL_ERR_INVALID_STATE;
    }
    memset(&key, 0, sizeof(key));
    if (!rc) rc = h2_gizclaw_api_key_state_close(f->device_ble_key);
    uint64_t start = 0u;
    if (!rc) rc = h2_pal_time_get_monotonic_ms(f->time, &start);
    while (!rc) {
      int next = h2_gizclaw_api_key_state_destroy(&f->device_ble_key);
      if (next != H2_PAL_ERR_BUSY) {
        rc = next;
        break;
      }
      rc = poll_service(f);
      if (!rc) rc = elapsed(f, start, KEY_WAIT_MS);
    }
  }
  return rc;
}
static int emit(const char* method, const char* stage, int rc) {
  char symbol[80];
  (void)snprintf(symbol, sizeof(symbol), "h2_gizclaw_ble_binding_%s", method);
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}
int h2_gizclaw_e2e_run_ble_binding(h2_gizclaw_e2e_fixture_t* f) {
  if (!f || !f->runtime->ble_host || !f->runtime->system_event)
    return H2_PAL_ERR_UNSUPPORTED;
  if (f->device_ble_binding || f->device_ble_key)
    return H2_PAL_ERR_INVALID_STATE;
  f->device_ble_cleanup = h2_gizclaw_e2e_ble_binding_cleanup;
  h2_gizclaw_api_key_state_config_t kc = {
      .service = f->actors[0].service,
      .mem = f->allocator,
      .sync = f->runtime->sync,
      .time = f->time,
      .display_name = {"gizos e2e BLE temporary", 23u},
      .timeout_ms = KEY_WAIT_MS};
  int rc = h2_gizclaw_api_key_state_create(&kc, &f->device_ble_key);
  if (!rc)
    rc = h2_gizclaw_api_key_state_request_refresh(f->device_ble_key, false);
  h2_gizclaw_api_key_snapshot_t key = {0};
  if (!rc) rc = key_idle(f, &key);
  if (!rc && (!key.valid || key.stale || key.closed || key.last_error))
    rc = H2_PAL_ERR_INVALID_STATE;
  uint8_t nonce[8] = {0};
  if (!rc) rc = h2_pal_crypto_random(f->crypto, nonce, sizeof(nonce));
  char local_name[30];
  (void)snprintf(local_name, sizeof(local_name),
                 "h2e2e-%02x%02x%02x%02x%02x%02x%02x%02x", nonce[0], nonce[1],
                 nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
  const h2_gizclaw_ble_binding_config_t config = {
      .api_key_state = f->device_ble_key,
      .ble = f->runtime->ble_host,
      .system_event = f->runtime->system_event,
      .mem = f->allocator,
      .sync = f->runtime->sync,
      .server_origin = h2_gizclaw_e2e_str(f->config->device_api_url),
      .icon = {"gizos", 5u},
      .name = {"GizOS E2E", 9u},
      .local_name = h2_gizclaw_e2e_str(local_name)};
  char url[H2_GIZCLAW_BLE_BINDING_URL_MAX + 1u] = {0};
  char expected[sizeof(url)] = {0};
  size_t length = 0u;
  if (!rc)
    rc = emit(
        "format_url", "ble-binding",
        h2_gizclaw_ble_binding_format_url(
            config.server_origin, config.icon, config.name,
            h2_gizclaw_e2e_str(key.key.secret), url, sizeof(url), &length));
  int count = snprintf(expected, sizeof(expected),
                       "%s/api-keys/%s?icon=gizos&name=GizOS%%20E2E",
                       f->config->device_api_url, key.key.secret);
  if (!rc && (count <= 0 || (size_t)count != length || strcmp(url, expected)))
    rc = H2_PAL_ERR_INVALID_STATE;
  emit("format_url", "ble_binding_format_url-assert", rc);
  memset(url, 0, sizeof(url));
  memset(expected, 0, sizeof(expected));
  h2_gizclaw_ble_binding_snapshot_t snapshot = {0};
  if (!rc)
    rc = emit("open", "ble-binding",
              h2_gizclaw_ble_binding_open(&config, &f->device_ble_binding));
  if (!rc)
    rc =
        emit("snapshot", "ble-binding",
             h2_gizclaw_ble_binding_snapshot(f->device_ble_binding, &snapshot));
  if (!rc && (snapshot.open || snapshot.advertising || snapshot.ready ||
              snapshot.url_len || snapshot.connected ||
              snapshot.pending_exposures || snapshot.revision != key.revision ||
              snapshot.info_flags != H2_GIZCLAW_BLE_BINDING_CLOSED))
    rc = H2_PAL_ERR_INVALID_STATE;
  emit("snapshot", "ble_binding_open-assert", rc);
  if (!rc)
    rc = emit("start", "ble-binding",
              h2_gizclaw_ble_binding_start(f->device_ble_binding));
  if (!rc)
    rc =
        emit("snapshot", "ble-binding",
             h2_gizclaw_ble_binding_snapshot(f->device_ble_binding, &snapshot));
  if (!rc && (!snapshot.open || !snapshot.advertising || !snapshot.ready ||
              snapshot.url_len != length || snapshot.revision != key.revision ||
              snapshot.info_flags != H2_GIZCLAW_BLE_BINDING_READY ||
              snapshot.last_error))
    rc = H2_PAL_ERR_INVALID_STATE;
  emit("snapshot", "ble_binding_start-assert", rc);
  /* Evidence is frozen until the complete run ends. Publish only this public
   * peer rendezvous immediately, without credential or key-name bytes. */
  char peer_line[256];
  (void)snprintf(peer_line, sizeof(peer_line),
                 "H2_GIZCLAW_BLE_WINDOW stage=setup rc=%d", rc);
  (void)h2_pal_log_write(f->log, H2_PAL_LOG_INFO, "gizclaw-ble", peer_line);
  if (!rc) {
    (void)snprintf(
        peer_line, sizeof(peer_line),
        "H2_GIZCLAW_BLE_PEER local_name=%s revision=%llu url_bytes=%zu window_ms=%u",
        local_name, (unsigned long long)key.revision, length, PEER_WAIT_MS);
    (void)h2_pal_log_write(f->log, H2_PAL_LOG_INFO, "gizclaw-ble", peer_line);
    h2_gizclaw_e2e_emit(
        "H2_GIZCLAW_E2E stage=ble-binding-peer local_name=%s revision=%llu url_bytes=%zu window_ms=%u\n",
        local_name, (unsigned long long)key.revision, length, PEER_WAIT_MS);
  }
  uint64_t start = 0u;
  bool exposed = false;
  if (!rc) rc = h2_pal_time_get_monotonic_ms(f->time, &start);
  while (!rc) {
    rc = h2_gizclaw_ble_binding_poll(f->device_ble_binding);
    if (!rc)
      rc = h2_gizclaw_ble_binding_snapshot(f->device_ble_binding, &snapshot);
    if (!rc && !exposed) {
      h2_gizclaw_ble_binding_exposure_t exposure = {0};
      int next = h2_gizclaw_ble_binding_next_exposure(f->device_ble_binding,
                                                      &exposure);
      if (next != H2_PAL_ERR_WOULD_BLOCK) {
        rc = emit("next_exposure", "ble-binding", next);
        if (!rc && (exposure.revision != key.revision ||
                    strcmp(exposure.key_name, key.key.name)))
          rc = H2_PAL_ERR_INVALID_STATE;
        emit("next_exposure", "ble_binding_next_exposure-assert", rc);
        exposed = !rc;
      }
    }
    if (!rc && exposed && !snapshot.connected && snapshot.advertising) break;
    if (!rc) rc = elapsed(f, start, PEER_WAIT_MS);
    if (!rc) rc = poll_service(f);
  }
  if (!rc)
    rc = emit("poll", "ble-binding",
              h2_gizclaw_ble_binding_poll(f->device_ble_binding));
  if (!rc)
    rc =
        emit("snapshot", "ble-binding",
             h2_gizclaw_ble_binding_snapshot(f->device_ble_binding, &snapshot));
  if (!rc && (!exposed || !snapshot.open || !snapshot.advertising ||
              snapshot.connected || !snapshot.ready ||
              snapshot.revision != key.revision || snapshot.url_len != length ||
              snapshot.pending_exposures))
    rc = H2_PAL_ERR_INVALID_STATE;
  emit("snapshot", "ble_binding_poll-assert", rc);
  if (!rc)
    rc =
        emit("snapshot", "ble-binding",
             h2_gizclaw_ble_binding_snapshot(f->device_ble_binding, &snapshot));
  if (!rc && (!snapshot.ready || snapshot.connected || snapshot.last_error))
    rc = H2_PAL_ERR_INVALID_STATE;
  emit("snapshot", "ble_binding_snapshot-assert", rc);
  if (!rc)
    rc = emit("stop", "ble-binding",
              h2_gizclaw_ble_binding_stop(f->device_ble_binding));
  if (!rc)
    rc =
        emit("snapshot", "ble-binding",
             h2_gizclaw_ble_binding_snapshot(f->device_ble_binding, &snapshot));
  if (!rc &&
      (snapshot.open || snapshot.advertising || snapshot.connected ||
       snapshot.ready || snapshot.url_len || snapshot.pending_exposures ||
       snapshot.info_flags != H2_GIZCLAW_BLE_BINDING_CLOSED))
    rc = H2_PAL_ERR_INVALID_STATE;
  emit("snapshot", "ble_binding_stop-assert", rc);
  if (!rc)
    rc = emit("close", "ble-binding",
              h2_gizclaw_ble_binding_close(&f->device_ble_binding));
  if (!rc && f->device_ble_binding) rc = H2_PAL_ERR_INVALID_STATE;
  emit("close", "ble_binding_close-assert", rc);
  memset(&key, 0, sizeof(key));
  int cleaned = h2_gizclaw_e2e_ble_binding_cleanup(f);
  h2_gizclaw_e2e_emit(
      "H2_GIZCLAW_E2E stage=ble-binding-cleanup rc=%d retained=%u\n", cleaned,
      (unsigned)(f->device_ble_binding != NULL || f->device_ble_key != NULL));
  return rc ? rc : cleaned;
}
