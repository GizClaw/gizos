#include "h2_gizclaw_api_key.h"
#include "h2_gizclaw_e2e_catalog.h"
#include "h2_gizclaw_telemetry.h"
#include "h2_yyjson_json.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Virtual device sink: validates real decoded PCM without making host audio
 * output or physical flash writes a prerequisite of the API acceptance lane. */
static atomic_uint volume;
static atomic_ullong pcm_bytes;
static atomic_uint pcm_peak;
static atomic_ullong stage_bytes;
static h2_pal_audio_track_t track;
static h2_pal_audio_track_t *real_track;
static const h2_pal_audio_api_t *real_audio;
static const h2_pal_time_api_t *sink_time;
static int volume_get(void *user, uint32_t *out) {
  (void)user;
  if (real_audio)
    return h2_pal_audio_get_speaker_volume_percent(real_audio, out);
  *out = atomic_load(&volume);
  return H2_PAL_OK;
}
static int volume_set(void *user, uint32_t value) {
  (void)user;
  atomic_store(&volume, value);
  return real_audio ? h2_pal_audio_set_speaker_volume_percent(real_audio, value)
                    : H2_PAL_OK;
}
static int audio_info(void *user, h2_audio_info_t *out) {
  (void)user;
  if (real_audio)
    return h2_pal_audio_get_info(real_audio, out);
  *out = (h2_audio_info_t){
      .available = 1,
      .playback_supported = 1,
      .playback_format = {.sample_rate_hz = 16000,
                          .frame_samples_per_channel = 320,
                          .channels = 1,
                          .sample_format = H2_AUDIO_SAMPLE_S16LE}};
  return H2_PAL_OK;
}
static int speaker_start(void *user) {
  (void)user;
  return real_audio ? h2_pal_audio_start_speaker(real_audio) : H2_PAL_OK;
}
static int pcm_write(h2_pal_audio_track_t *t, const h2_audio_frame_t *frame,
                     uint32_t timeout) {
  (void)t;
  (void)timeout;
  if (frame->sample_rate_hz != 16000 || frame->channels != 1 ||
      frame->bytes != (size_t)frame->samples_per_channel * 2)
    return H2_PAL_ERR_FORMAT;
  if (real_track) {
    int rc = h2_pal_audio_track_write(real_track, frame, timeout);
    if (rc == H2_PAL_OK) {
      unsigned peak = 0;
      const uint8_t *pcm = frame->data;
      for (size_t i = 0; i < frame->bytes; i += 2) {
        int value = (int16_t)((unsigned)pcm[i] | ((unsigned)pcm[i + 1] << 8));
        unsigned amplitude = (unsigned)(value < 0 ? -value : value);
        if (amplitude > peak)
          peak = amplitude;
      }
      if (peak > atomic_load(&pcm_peak))
        atomic_store(&pcm_peak, peak);
      atomic_fetch_add(&pcm_bytes, frame->bytes);
    }
    return rc;
  }
  atomic_fetch_add(&pcm_bytes, frame->bytes);
  return h2_pal_time_sleep_ms(sink_time, (uint32_t)(frame->bytes / 32));
}
static int pcm_drain(h2_pal_audio_track_t *t, uint32_t timeout) {
  (void)t;
  return real_track ? h2_pal_audio_track_drain(real_track, timeout) : H2_PAL_OK;
}
static int pcm_close(h2_pal_audio_track_t *t) {
  (void)t;
  int rc = real_track ? h2_pal_audio_track_close(real_track) : H2_PAL_OK;
  real_track = NULL;
  return rc;
}
static int track_create(void *user, const h2_audio_track_config_t *config,
                        h2_pal_audio_track_t **out) {
  (void)user;
  if (real_audio) {
    int rc = h2_pal_audio_create_track(real_audio, config, &real_track);
    if (rc != H2_PAL_OK)
      return rc;
  }
  track = (h2_pal_audio_track_t){
      .write = pcm_write, .drain = pcm_drain, .close = pcm_close};
  *out = &track;
  return H2_PAL_OK;
}
static const h2_pal_audio_vtable_t audio_vtable = {
    .get_info = audio_info,
    .start_speaker = speaker_start,
    .get_speaker_volume_percent = volume_get,
    .set_speaker_volume_percent = volume_set,
    .create_track = track_create};
static const h2_pal_audio_api_t audio = {.vtable = &audio_vtable};
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
             (unsigned long long)atomic_load(&pcm_bytes),
             atomic_load(&pcm_peak));
    if (!first_playing_at && !strcmp(local.state, "playing") &&
        local.position_ms)
      first_playing_at = now;
    if (!local_played && !strcmp(local.state, "playing") &&
        local.position_ms >=
            (fixture->config->device_real_audio ? 20000u : 20u) &&
        atomic_load(&pcm_bytes)) {
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
  ASSERT(atomic_load(&volume) == 37);
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
        atomic_load(&pcm_bytes) > 0) {
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
         (unsigned long long)atomic_load(&pcm_bytes),
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
  sink_time = fixture->time;
  real_audio =
      fixture->config->device_real_audio ? fixture->runtime->audio : NULL;
  if (fixture->config->device_real_audio && !real_audio)
    return H2_PAL_ERR_UNSUPPORTED;
  if (real_audio) {
    int rc = h2_pal_audio_set_speaker_volume_percent(real_audio, 100);
    if (rc != H2_PAL_OK)
      return rc;
  }
  atomic_store(&pcm_bytes, 0);
  atomic_store(&pcm_peak, 0);
  atomic_store(&stage_bytes, 0);
  atomic_store(&volume, real_audio ? 100 : 50);
  fixture->device_audio = &audio;
  fixture->device_vtable = &device_vtable;
  return H2_PAL_OK;
}
