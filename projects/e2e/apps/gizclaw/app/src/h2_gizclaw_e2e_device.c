#include "h2_gizclaw_api_key.h"
#include "h2_gizclaw_e2e_catalog.h"
#include "h2_gizclaw_telemetry.h"
#include "h2_yyjson_json.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* OTA rejection is a GizClaw case policy; Audio support belongs to app_test. */
static atomic_ullong stage_bytes;
static h2_app_test_audio_evidence_t device_evidence(h2_gizclaw_e2e_fixture_t *fixture) {
  h2_app_test_audio_evidence_t evidence = {0};
  (void)h2_app_test_audio_copy_evidence(fixture->device_audio_wrapper, &evidence);
  return evidence;
}
static int dispose_device_audio(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture->device_audio_wrapper) {
    if (device_evidence(fixture).speaker_active) {
      int rc = h2_pal_audio_stop_speaker(fixture->device_audio);
      if (rc != H2_PAL_OK) return rc;
    }
    int rc = h2_app_test_audio_destroy(fixture->device_audio_wrapper);
    if (rc != H2_PAL_OK) return rc;
    fixture->device_audio_wrapper = NULL;
    fixture->device_audio = NULL;
  }
  int rc = h2_app_test_audio_fake_deinit(&fixture->device_audio_fake);
  if (rc == H2_PAL_OK) fixture->device_cleanup = NULL;
  return rc;
}
static h2_pal_result_t stage_begin(void *user,
                                   const struct h2_gizclaw_firmware *firmware,
                                   const char *update_id) {
  (void)user;
  (void)firmware;
  (void)update_id;
  atomic_store(&stage_bytes, 0);
  return H2_PAL_OK;
}
static h2_pal_result_t stage_write(void *user, const uint8_t *data,
                                   size_t length) {
  (void)user;
  (void)data;
  atomic_fetch_add(&stage_bytes, length);
  return H2_PAL_OK;
}
static h2_pal_result_t stage_finish(void *user) {
  (void)user;
  return H2_PAL_ERR_FORMAT;
}
static void stage_abort(void *user) { (void)user; }
static h2_pal_result_t stage_activate(void *user) {
  (void)user;
  return H2_PAL_ERR_INVALID_STATE;
}
static const h2_gizclaw_vtable_t device_vtable = {.ota_begin = stage_begin,
                                                  .ota_write = stage_write,
                                                  .ota_finish = stage_finish,
                                                  .ota_abort = stage_abort,
                                                  .ota_activate =
                                                      stage_activate};

typedef struct api_test {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_api_key_t key;
  h2_yyjson_json_t *provider;
  const h2_pal_json_api_t *json;
  h2_pal_json_document_t *document;
  h2_pal_json_value_t *root;
} api_test_t;
static int api_call(api_test_t *test, int method, const char *path,
                    const char *body, int expected) {
  (void)h2_pal_json_document_destroy(test->json, &test->document);
  test->root = NULL;
  char url[2048], authorization[112];
  int count = snprintf(url, sizeof(url), "%s/gizclaw/v1%s",
                       test->fixture->config->device_api_url, path);
  if (count < 0 || (size_t)count >= sizeof(url))
    return H2_PAL_ERR_INVALID_ARG;
  (void)snprintf(authorization, sizeof(authorization), "Bearer %s",
                 test->key.secret);
  const h2_pal_http_header_t headers[] = {
      {{"Authorization", 13}, {authorization, strlen(authorization)}},
      {{"Content-Type", 12}, {"application/json", 16}},
  };
  uint8_t bytes[16384];
  h2_pal_http_request_t request = {.method = method,
                                   .url = {url, strlen(url)},
                                   .headers = headers,
                                   .header_count = 2,
                                   .timeout_ms = 15000,
                                   .body = (const uint8_t *)body,
                                   .body_len = body ? strlen(body) : 0,
                                   .response_buf = bytes,
                                   .response_buf_cap = sizeof(bytes),
                                   .allocator = test->fixture->allocator};
  h2_pal_http_response_t response = {0};
  int rc = h2_pal_http_request(test->fixture->http, &request, &response);
  printf("H2_GIZCLAW_E2E stage=device-api path=%s http=%d expected=%d rc=%d\n",
         path, response.status_code, expected, rc);
  if (rc == H2_PAL_OK && response.status_code != expected)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK && response.body_len) {
    rc = h2_pal_json_document_parse(test->json, response.body,
                                    response.body_len, NULL, &test->document);
    if (rc == H2_PAL_OK)
      rc = h2_pal_json_document_root(test->json, test->document, &test->root);
  }
  h2_pal_http_response_free(test->fixture->http, &response);
  memset(authorization, 0, sizeof(authorization));
  return rc;
}
static h2_pal_json_value_t *field(api_test_t *test, const char *path) {
  h2_pal_json_value_t *value = test->root;
  while (value && *path) {
    size_t len = strcspn(path, ".");
    h2_pal_json_value_t *child = NULL;
    if (h2_pal_json_object_get(test->json, value, path, len, &child) !=
        H2_PAL_OK)
      return NULL;
    value = child;
    path += len;
    if (*path == '.')
      ++path;
  }
  return value;
}
static bool text_is(api_test_t *test, const char *path, const char *expected) {
  h2_pal_json_string_view_t value = {0};
  return h2_pal_json_value_get_string(test->json, field(test, path), &value) ==
             H2_PAL_OK &&
         value.len == strlen(expected) &&
         !memcmp(value.data, expected, value.len);
}
static bool number_is(api_test_t *test, const char *path, double expected) {
  double value = 0;
  return h2_pal_json_value_get_number(test->json, field(test, path), &value) ==
             H2_PAL_OK &&
         value == expected;
}
int h2_gizclaw_e2e_run_device(h2_gizclaw_e2e_fixture_t *fixture) {
  if (!fixture->config->device_api_url || !fixture->config->device_audio_url ||
      strncmp(fixture->config->device_api_url, "https://", 8) ||
      strncmp(fixture->config->device_audio_url, "https://", 8))
    return H2_PAL_ERR_INVALID_ARG;
  api_test_t test = {.fixture = fixture};
  int rc = h2_yyjson_json_create(fixture->allocator, &test.provider);
  if (rc != H2_PAL_OK)
    return rc;
  test.json = h2_yyjson_json_api(test.provider);

  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_rpc_api_key_create(service,
                                       h2_gizclaw_e2e_str("gizos-device-e2e"),
                                       false, 15000, &test.key);
  printf("H2_GIZCLAW_E2E stage=device-api-key-create rc=%d\n", rc);
#define CHECK(call)                                                            \
  do {                                                                         \
    if (rc == H2_PAL_OK)                                                       \
      rc = (call);                                                             \
  } while (0)
#define ASSERT(condition)                                                      \
  do {                                                                         \
    if (rc == H2_PAL_OK && !(condition))                                       \
      rc = H2_PAL_ERR_INVALID_STATE;                                           \
  } while (0)
  CHECK(h2_gizclaw_player_play(
      service, h2_gizclaw_e2e_str(fixture->config->device_audio_url)));
  h2_gizclaw_e2e_evidence("h2_gizclaw_player_play", "local-player", rc);
  h2_gizclaw_player_status_t local = {0};
  bool local_played = false;
  bool local_ended = false;
  uint64_t first_playing_at = 0;
  for (unsigned i = 0;
       rc == H2_PAL_OK && i < (fixture->config->device_real_audio ? 600u : 40u);
       ++i) {
    CHECK(h2_pal_time_sleep_ms(fixture->time, 250));
    CHECK(h2_gizclaw_player_get_status(service, &local));
    h2_gizclaw_e2e_evidence("h2_gizclaw_player_get_status", "local-player", rc);
    uint64_t now = 0;
    CHECK(h2_pal_time_get_monotonic_ms(fixture->time, &now));
    if (fixture->config->device_real_audio && i % 4u == 0)
      printf("H2_GIZCLAW_E2E stage=player-status state=%s position_ms=%llu "
             "pcm_bytes=%llu peak=%u\n",
             local.state, (unsigned long long)local.position_ms,
             (unsigned long long)device_evidence(fixture).playback_bytes,
             (unsigned)device_evidence(fixture).playback_peak);
    if (!first_playing_at && !strcmp(local.state, "playing") &&
        local.position_ms)
      first_playing_at = now;
    if (!local_played && !strcmp(local.state, "playing") &&
        local.position_ms >=
            (fixture->config->device_real_audio ? 20000u : 20u) &&
        device_evidence(fixture).playback_bytes) {
      CHECK(api_call(&test, H2_PAL_HTTP_GET, "/device/status", NULL, 200));
      if (text_is(&test, "audioplayer.state", "playing")) {
        local_played = true;
        printf("H2_GIZCLAW_E2E stage=player-cadence position_ms=%llu "
               "elapsed_ms=%llu\n",
               (unsigned long long)local.position_ms,
               (unsigned long long)(now - first_playing_at));
        if (fixture->config->device_real_audio)
          ASSERT(first_playing_at && now - first_playing_at <= 27000u);
        else
          break;
      }
    }
    if (fixture->config->device_real_audio && !strcmp(local.state, "ended")) {
      local_ended = true;
      ASSERT(local_played && local.has_duration_ms &&
             local.duration_ms == local.position_ms && local.position_ms > 0 &&
             now - first_playing_at <= local.position_ms + 10000u);
      printf("H2_GIZCLAW_E2E stage=player-ended position_ms=%llu "
             "elapsed_ms=%llu\n",
             (unsigned long long)local.position_ms,
             (unsigned long long)(now - first_playing_at));
      break;
    }
    if (!strcmp(local.state, "error")) {
      printf("H2_GIZCLAW_E2E stage=local-player-error code=%s message=%s\n",
             local.error_code, local.error_message);
      rc = H2_PAL_ERR_IO;
      break;
    }
  }
  ASSERT(local_played && (!fixture->config->device_real_audio || local_ended));
  h2_gizclaw_e2e_evidence("h2_gizclaw_player_play", "player_play-assert", rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_player_get_status",
                          "player_get_status-assert", rc);
  CHECK(h2_gizclaw_player_stop(service));
  h2_gizclaw_e2e_evidence("h2_gizclaw_player_stop", "local-player", rc);
  CHECK(h2_gizclaw_player_get_status(service, &local));
  ASSERT(!strcmp(local.state, "stopped"));
  h2_gizclaw_e2e_evidence("h2_gizclaw_player_stop", "player_stop-assert", rc);
  CHECK(h2_gizclaw_ota_start(service, H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP,
                             (h2_gizclaw_str_t){0}));
  h2_gizclaw_e2e_evidence("h2_gizclaw_ota_start", "local-ota", rc);
  bool local_failed = false;
  h2_gizclaw_ota_status_t ota_status = {0};
  char local_update_id[65] = {0};
  for (unsigned i = 0; rc == H2_PAL_OK && i < 60; ++i) {
    CHECK(h2_pal_time_sleep_ms(fixture->time, 500));
    CHECK(api_call(&test, H2_PAL_HTTP_GET, "/device/status", NULL, 200));
    CHECK(h2_gizclaw_ota_get_status(service, &ota_status));
    h2_gizclaw_e2e_evidence("h2_gizclaw_ota_get_status", "local-ota", rc);
    if (text_is(&test, "ota.state", "failed") &&
        ota_status.phase == H2_GIZCLAW_OTA_FAILED) {
      h2_pal_json_string_view_t id = {0};
      if (h2_pal_json_value_get_string(test.json, field(&test, "ota.update_id"),
                                       &id) == H2_PAL_OK &&
          id.len > 0 && id.len < sizeof(local_update_id)) {
        memcpy(local_update_id, id.data, id.len);
        local_failed = true;
      }
      break;
    }
  }
  ASSERT(local_failed && atomic_load(&stage_bytes) > 0 &&
         ota_status.result != H2_PAL_OK);
  h2_gizclaw_e2e_evidence("h2_gizclaw_ota_get_status", "ota_get_status-assert", rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_ota_start", "ota_start-assert", rc);
  int first_failure = rc;
  /* Exercise independent reverse-RPC scenarios even if another scenario fails;
   * keep the first failure so partial success cannot make this lane green. */
  if (test.key.name[0])
    rc = H2_PAL_OK;
  CHECK(api_call(&test, H2_PAL_HTTP_PUT, "/device/volume",
                 "{\"level\":37,\"muted\":false}", 200));
  uint32_t volume = 0u;
  CHECK(h2_pal_audio_get_speaker_volume_percent(fixture->device_audio, &volume));
  ASSERT(volume == 37u);
  if (first_failure == H2_PAL_OK)
    first_failure = rc;
  if (test.key.name[0])
    rc = H2_PAL_OK;
  CHECK(api_call(&test, H2_PAL_HTTP_PUT, "/device/audioplayer/playlist",
                 "{\"items\":[{\"url\":\"http://invalid.test/music.ogg\"}]}",
                 400));
  char playlist[1400];
  int n = snprintf(playlist, sizeof(playlist), "{\"items\":[{\"url\":\"%s\"}]}",
                   fixture->config->device_audio_url);
  ASSERT(n > 0 && (size_t)n < sizeof(playlist) &&
         !strchr(fixture->config->device_audio_url, '"'));
  CHECK(api_call(&test, H2_PAL_HTTP_PUT, "/device/audioplayer/playlist",
                 playlist, 200));
  ASSERT(number_is(&test, "status.playlist_length", 1));
  CHECK(api_call(&test, H2_PAL_HTTP_PUT, "/device/audioplayer/mode",
                 "{\"repeat\":\"one\"}", 200));
  CHECK(api_call(&test, H2_PAL_HTTP_POST, "/device/audioplayer/actions/play",
                 "{\"index\":0}", 200));
  bool played = false;
  for (unsigned i = 0; rc == H2_PAL_OK && i < 30; ++i) {
    CHECK(h2_pal_time_sleep_ms(fixture->time, 500));
    CHECK(api_call(&test, H2_PAL_HTTP_GET, "/device/status", NULL, 200));
    if (text_is(&test, "audioplayer.state", "playing") &&
        device_evidence(fixture).playback_bytes > 0) {
      played = true;
      break;
    }
    if (text_is(&test, "audioplayer.state", "error")) {
      rc = H2_PAL_ERR_IO;
      break;
    }
  }
  ASSERT(played);
  CHECK(api_call(&test, H2_PAL_HTTP_POST, "/device/audioplayer/actions/stop",
                 NULL, 200));
  ASSERT(text_is(&test, "status.state", "stopped"));
  if (first_failure == H2_PAL_OK)
    first_failure = rc;
  if (test.key.name[0])
    rc = H2_PAL_OK;
  CHECK(api_call(&test, H2_PAL_HTTP_POST, "/device/actions/firmware-update",
                 "{\"channel\":\"develop\"}", 204));
  bool failed = false;
  for (unsigned i = 0; rc == H2_PAL_OK && i < 60; ++i) {
    CHECK(h2_pal_time_sleep_ms(fixture->time, 500));
    CHECK(api_call(&test, H2_PAL_HTTP_GET, "/device/status", NULL, 200));
    if (text_is(&test, "ota.state", "failed") &&
        !text_is(&test, "ota.update_id", local_update_id)) {
      failed = true;
      break;
    }
  }
  ASSERT(failed && atomic_load(&stage_bytes) > 0);
  if (first_failure == H2_PAL_OK)
    first_failure = rc;
  rc = first_failure;
  printf("H2_GIZCLAW_E2E stage=device-api-assert pcm_bytes=%llu "
         "stage_bytes=%llu result=%s rc=%d\n",
         (unsigned long long)device_evidence(fixture).playback_bytes,
         (unsigned long long)atomic_load(&stage_bytes),
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  if (test.key.name[0]) {
    (void)h2_gizclaw_player_stop(service);
    int cleanup = h2_gizclaw_rpc_api_key_revoke(
        service, h2_gizclaw_e2e_str(test.key.name), 15000);
    printf("H2_GIZCLAW_E2E stage=device-api-key-revoke rc=%d\n", cleanup);
    if (rc == H2_PAL_OK)
      rc = cleanup;
  }
  memset(&test.key, 0, sizeof(test.key));
  (void)h2_pal_json_document_destroy(test.json, &test.document);
  (void)h2_yyjson_json_destroy(&test.provider);
  return rc;
#undef CHECK
#undef ASSERT
}
int h2_gizclaw_e2e_prepare_device(h2_gizclaw_e2e_fixture_t *fixture) {
  const h2_pal_audio_api_t *delegate = fixture->runtime->audio;
  int rc = H2_PAL_OK;
  fixture->device_cleanup = dispose_device_audio;
  if (!fixture->config->device_real_audio) {
    rc = h2_app_test_audio_fake_init(&fixture->device_audio_fake, fixture->allocator);
    if (rc != H2_PAL_OK) return rc;
    fixture->device_audio_fake.playback_time = fixture->time;
    delegate = &fixture->device_audio_fake.api;
  }
  if (!delegate) return H2_PAL_ERR_UNSUPPORTED;
  rc = h2_app_test_audio_create(fixture->allocator, fixture->time, delegate,
                               NULL, &fixture->device_audio_wrapper);
  if (rc != H2_PAL_OK) return rc;
  fixture->device_audio = h2_app_test_audio_api(fixture->device_audio_wrapper);
  rc = h2_pal_audio_set_speaker_volume_percent(fixture->device_audio,
      fixture->config->device_real_audio ? 100u : 50u);
  if (rc != H2_PAL_OK) return rc;
  atomic_store(&stage_bytes, 0);
  fixture->device_vtable = &device_vtable;
  return H2_PAL_OK;
}
