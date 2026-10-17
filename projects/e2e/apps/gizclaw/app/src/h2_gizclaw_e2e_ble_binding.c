#include "h2_gizclaw_e2e_ble_binding.h"

#include "h2_gizclaw_ble_binding.h"
#include "h2_sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { BINDING_WAIT_MS = 60000u };

static void erase(void *bytes, size_t size) {
  volatile unsigned char *p = bytes;
  while (size-- != 0u) *p++ = 0u;
}

static int pause_case(h2_gizclaw_e2e_fixture_t *fixture, uint64_t deadline) {
  uint64_t now = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &now);
  if (rc != H2_PAL_OK) return rc;
  if (now >= deadline || !h2_gizclaw_e2e_fixture_has_time(fixture, 20u))
    return H2_PAL_ERR_TIMEOUT;
  return h2_gizclaw_e2e_fixture_poll(fixture, H2_GIZCLAW_E2E_OWNER, 20u);
}

static int wait_key(h2_gizclaw_e2e_fixture_t *fixture,
                    h2_gizclaw_api_key_snapshot_t *snapshot, bool valid) {
  uint64_t now = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &now);
  if (rc != H2_PAL_OK || UINT64_MAX - now < BINDING_WAIT_MS)
    return rc != H2_PAL_OK ? rc : H2_PAL_ERR_INVALID_STATE;
  const uint64_t deadline = now + BINDING_WAIT_MS;
  do {
    rc = h2_gizclaw_api_key_state_snapshot(fixture->actors[0].api_key_state,
                                           snapshot);
    if (rc != H2_PAL_OK) return rc;
    if (!snapshot->busy)
      return snapshot->last_error == H2_PAL_OK && snapshot->valid == valid &&
                     !snapshot->stale && !snapshot->closed
                 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
    rc = pause_case(fixture, deadline);
  } while (rc == H2_PAL_OK);
  return rc;
}

enum binding_wait { EXPOSED, DISCONNECTED, CONNECTED };

static int wait_binding(h2_gizclaw_e2e_fixture_t *fixture,
                        enum binding_wait condition,
                        h2_gizclaw_ble_binding_snapshot_t *snapshot) {
  uint64_t now = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &now);
  if (rc != H2_PAL_OK || UINT64_MAX - now < BINDING_WAIT_MS)
    return rc != H2_PAL_OK ? rc : H2_PAL_ERR_INVALID_STATE;
  const uint64_t deadline = now + BINDING_WAIT_MS;
  do {
    rc = h2_gizclaw_ble_binding_snapshot(fixture->actors[0].ble_binding,
                                         snapshot);
    if (rc != H2_PAL_OK) return rc;
    if (!snapshot->open || !snapshot->ready || snapshot->last_error != H2_PAL_OK)
      return H2_PAL_ERR_INVALID_STATE;
    if ((condition == EXPOSED && snapshot->pending_exposures != 0u) ||
        (condition == DISCONNECTED && !snapshot->connected && !snapshot->advertising) ||
        (condition == CONNECTED && snapshot->connected))
      return H2_PAL_OK;
    rc = pause_case(fixture, deadline);
  } while (rc == H2_PAL_OK);
  return rc;
}

static void proof(const char *method, int rc) {
  char symbol[96], stage[96];
  (void)snprintf(symbol, sizeof(symbol), "h2_gizclaw_ble_binding_%s", method);
  (void)snprintf(stage, sizeof(stage), "ble_binding_%s-assert", method);
  const bool observed = !strcmp(method, "open") || !strcmp(method, "start") ||
                        !strcmp(method, "poll") || !strcmp(method, "stop");
  h2_gizclaw_e2e_evidence(observed ? "h2_gizclaw_ble_binding_snapshot" : symbol,
                          stage, rc);
}

int h2_gizclaw_e2e_run_ble_binding(h2_gizclaw_e2e_fixture_t *fixture) {
  h2_gizclaw_e2e_actor_t *actor = &fixture->actors[0];
  if (!fixture->runtime->ble_host || !fixture->runtime->system_event ||
      actor->api_key_state || actor->ble_binding)
    return H2_PAL_ERR_INVALID_STATE;
  h2_gizclaw_api_key_state_config_t key_config = {
      .service = actor->service, .mem = fixture->allocator,
      .sync = fixture->runtime->sync, .time = fixture->time,
      .display_name = h2_gizclaw_e2e_str(fixture->run_prefix), .timeout_ms = 30000u};
  h2_gizclaw_api_key_snapshot_t key = {0};
  h2_gizclaw_ble_binding_snapshot_t snapshot = {0};
  h2_gizclaw_ble_binding_exposure_t exposure = {0};
  char url[H2_GIZCLAW_BLE_BINDING_URL_MAX + 1u] = {0};
  char expected[sizeof(url)] = {0}, sha_hex[65] = {0};
  size_t url_len = 0u;
  int rc = h2_gizclaw_api_key_state_create(&key_config, &actor->api_key_state);
#define CHECK(call) do { if (rc == H2_PAL_OK) rc = (call); } while (0)
#define REQUIRE(condition) do { if (rc == H2_PAL_OK && !(condition)) \
    rc = H2_PAL_ERR_INVALID_STATE; } while (0)
#define CALL(method, call) do { CHECK(call); \
    h2_gizclaw_e2e_evidence("h2_gizclaw_ble_binding_" method, "ble-binding", rc); } while (0)
#define SNAPSHOT() CALL("snapshot", h2_gizclaw_ble_binding_snapshot(actor->ble_binding, &snapshot))
  CHECK(h2_gizclaw_api_key_state_request_refresh(actor->api_key_state, false));
  CHECK(wait_key(fixture, &key, true));
  const h2_gizclaw_ble_binding_config_t config = {
      .api_key_state = actor->api_key_state, .ble = fixture->runtime->ble_host,
      .system_event = fixture->runtime->system_event, .mem = fixture->allocator,
      .sync = fixture->runtime->sync,
      /* device_api_url is explicitly supplied as an HTTPS origin. Never infer
       * the binding origin from the RPC endpoint or a source default. */
      .server_origin = h2_gizclaw_e2e_str(fixture->config->device_api_url),
      .icon = {"gizos", 5u}, .name = {"GizOS E2E", 9u},
      .local_name = h2_gizclaw_e2e_str(fixture->run_prefix)};
  CALL("format_url", h2_gizclaw_ble_binding_format_url(
      config.server_origin, config.icon, config.name,
      h2_gizclaw_e2e_str(key.key.secret), url, sizeof(url), &url_len));
  const int length = snprintf(expected, sizeof(expected),
      "%s/api-keys/%s?icon=gizos&name=GizOS%%20E2E",
      fixture->config->device_api_url, key.key.secret);
  REQUIRE(length > 0 && (size_t)length < sizeof(expected) &&
          url_len == (size_t)length && !memcmp(url, expected, url_len + 1u));
  proof("format_url", rc);
  if (rc == H2_PAL_OK) {
    uint8_t digest[32];
    h2_sha256_t sha;
    h2_sha256_init(&sha);
    h2_sha256_update(&sha, (const uint8_t *)url, url_len);
    h2_sha256_finish(&sha, digest);
    h2_sha256_hex(digest, sha_hex);
  }
  erase(url, sizeof(url)); erase(expected, sizeof(expected));
  erase(key.key.secret, sizeof(key.key.secret));
  CALL("open", h2_gizclaw_ble_binding_open(&config, &actor->ble_binding));
  SNAPSHOT();
  REQUIRE(!snapshot.open && !snapshot.advertising && !snapshot.connected &&
          !snapshot.ready && snapshot.pending_exposures == 0u &&
          snapshot.info_flags == H2_GIZCLAW_BLE_BINDING_CLOSED);
  proof("open", rc);
  CALL("start", h2_gizclaw_ble_binding_start(actor->ble_binding));
  SNAPSHOT();
  REQUIRE(snapshot.open && snapshot.advertising && snapshot.ready &&
          !snapshot.connected && snapshot.pending_exposures == 0u &&
          snapshot.revision == key.revision && snapshot.url_len == url_len &&
          snapshot.info_flags == H2_GIZCLAW_BLE_BINDING_READY);
  proof("start", rc); proof("snapshot", rc);
  if (rc == H2_PAL_OK)
    h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=ble-binding-ready window=%s "
        "revision=%" PRIu64 " url_bytes=%zu url_sha256=%s\n",
        fixture->run_prefix, key.revision, url_len, sha_hex);
  CHECK(wait_binding(fixture, EXPOSED, &snapshot));
  REQUIRE(snapshot.pending_exposures == 1u);
  CALL("next_exposure", h2_gizclaw_ble_binding_next_exposure(actor->ble_binding,
                                                            &exposure));
  REQUIRE(exposure.revision == key.revision &&
          !strcmp(exposure.key_name, key.key.name));
  proof("next_exposure", rc);
  CHECK(wait_binding(fixture, DISCONNECTED, &snapshot));
  CALL("poll", h2_gizclaw_ble_binding_poll(actor->ble_binding));
  SNAPSHOT();
  REQUIRE(snapshot.open && snapshot.advertising && snapshot.ready &&
          !snapshot.connected && snapshot.pending_exposures == 0u &&
          snapshot.revision == key.revision);
  /* A second actual central connection proves the restarted set was usable.
   * The central holds each completed transfer briefly before disconnecting. */
  CHECK(wait_binding(fixture, CONNECTED, &snapshot));
  CHECK(wait_binding(fixture, DISCONNECTED, &snapshot));
  proof("poll", rc);
  if (actor->ble_binding) {
    const int stopped = h2_gizclaw_ble_binding_stop(actor->ble_binding);
    h2_gizclaw_e2e_evidence("h2_gizclaw_ble_binding_stop", "ble-binding", stopped);
    if (rc == H2_PAL_OK) rc = stopped;
    SNAPSHOT();
    REQUIRE(!snapshot.open && !snapshot.advertising && !snapshot.connected &&
            !snapshot.ready && snapshot.url_len == 0u &&
            snapshot.pending_exposures == 0u &&
            snapshot.info_flags == H2_GIZCLAW_BLE_BINDING_CLOSED);
    proof("stop", rc);
    /* A failed run may have undrained possible exposures. Seal first, then
     * drain before close; the owned temporary key is explicitly revoked. */
    int drained = H2_PAL_OK;
    if (stopped == H2_PAL_OK) {
      while ((drained = h2_gizclaw_ble_binding_next_exposure(actor->ble_binding,
                                                            &exposure)) == H2_PAL_OK)
        erase(&exposure, sizeof(exposure));
      const int closed = drained == H2_PAL_ERR_WOULD_BLOCK
                             ? h2_gizclaw_ble_binding_close(&actor->ble_binding)
                             : drained;
      h2_gizclaw_e2e_evidence("h2_gizclaw_ble_binding_close", "ble-binding", closed);
      if (rc == H2_PAL_OK) rc = closed;
      REQUIRE(actor->ble_binding == NULL);
      proof("close", rc);
    }
  }
  if (actor->api_key_state && actor->ble_binding == NULL) {
    int cleanup = h2_gizclaw_api_key_state_request_revoke(actor->api_key_state);
    if (cleanup == H2_PAL_OK) cleanup = wait_key(fixture, &key, false);
    if (cleanup == H2_PAL_OK) cleanup = h2_gizclaw_api_key_state_close(actor->api_key_state);
    if (cleanup == H2_PAL_OK) cleanup = h2_gizclaw_api_key_state_destroy(&actor->api_key_state);
    h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=ble-binding-key-cleanup rc=%d\n", cleanup);
    if (rc == H2_PAL_OK) rc = cleanup;
  }
  erase(&key, sizeof(key)); erase(&exposure, sizeof(exposure));
  return rc;
#undef SNAPSHOT
#undef CALL
#undef REQUIRE
#undef CHECK
}
