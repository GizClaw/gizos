#include "h2_gizclaw_e2e_speech.h"
#include "h2_yyjson_json.h"

#include <string.h>

static bool contains_blue(const char *text) {
  if (text == NULL)
    return false;
  const size_t len = strlen(text);
  for (size_t index = 0u; index + 4u <= len; ++index) {
    const char *cursor = text + index;
    if ((cursor[0] == 'b' || cursor[0] == 'B') &&
        (cursor[1] == 'l' || cursor[1] == 'L') &&
        (cursor[2] == 'u' || cursor[2] == 'U') &&
        (cursor[3] == 'e' || cursor[3] == 'E')) {
      return true;
    }
  }
  return strstr(text, "蓝") != NULL;
}

static bool speech_view_has_blue(h2_gizclaw_str_t value) {
  char text[1024];
  if (value.data == NULL || value.len == 0u || value.len >= sizeof(text) ||
      memchr(value.data, '\0', value.len) != NULL)
    return false;
  memcpy(text, value.data, value.len);
  text[value.len] = '\0';
  return contains_blue(text);
}

int h2_gizclaw_e2e_validate_color_json(const h2_pal_mem_api_t *allocator,
                                       h2_gizclaw_str_t value) {
  h2_yyjson_json_t *provider = NULL;
  int rc = h2_yyjson_json_create(allocator, &provider);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_json_api_t *json = h2_yyjson_json_api(provider);
  h2_pal_json_document_t *document = NULL;
  h2_pal_json_value_t *root = NULL, *color = NULL;
  size_t count = 0u;
  h2_pal_json_string_view_t text = {0};
  rc = h2_pal_json_document_parse(json, (const uint8_t *)value.data, value.len,
                                  NULL, &document);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_document_root(json, document, &root);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_object_size(json, root, &count);
  if (rc == H2_PAL_OK && count != 1u)
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_object_get(json, root, "color", 5u, &color);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_value_get_string(json, color, &text);
  if (rc == H2_PAL_OK && (text.len != 4u || text.data == NULL ||
                          memcmp(text.data, "blue", 4u) != 0))
    rc = H2_PAL_ERR_FORMAT;
  const int destroy_rc = h2_pal_json_document_destroy(json, &document);
  const int provider_rc = h2_yyjson_json_destroy(&provider);
  if (rc == H2_PAL_OK)
    rc = destroy_rc;
  return rc == H2_PAL_OK ? provider_rc : rc;
}

/* Application-side mic pump: one 20 ms frame per deadline. A full Track
 * retains this exact fixture prefix for the next attempt. */
static int speech_pump(h2_gizclaw_e2e_fixture_t *fixture) {
  const size_t offset =
      atomic_load_explicit(&fixture->speech_offset, memory_order_acquire);
  if (offset >= fixture->pcm_len)
    return H2_PAL_ERR_WOULD_BLOCK;
  const size_t remaining = fixture->pcm_len - offset;
  const size_t copied = remaining < 640u ? remaining : 640u;
  int rc = h2_gizclaw_pcm_track_write(fixture->speech_track,
                                       fixture->pcm + offset, copied);
  if (rc == H2_PAL_OK)
    atomic_store_explicit(&fixture->speech_offset, offset + copied,
                          memory_order_release);
  return rc;
}

/* A timed wait is observation only: it must not finish or cancel the body.
 * The net/uplink tasks continue without any application service_poll call. */
static int speech_wait(h2_gizclaw_e2e_fixture_t *fixture,
                       h2_gizclaw_req_t *request) {
  uint64_t started = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &started);
  if (rc != H2_PAL_OK)
    return rc;
  bool finished = false;
  uint64_t next_frame = started;
  for (;;) {
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(fixture->time, &now);
    if (rc != H2_PAL_OK)
      return rc;
    if (now < started || now - started >= 30000u)
      return H2_PAL_ERR_TIMEOUT;
    if (!h2_gizclaw_e2e_fixture_has_time(fixture, 10u))
      return H2_PAL_ERR_TIMEOUT;
    if (!finished && now >= next_frame) {
      rc = speech_pump(fixture);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK)
        return rc;
      next_frame = now + 20u;
    }
    if (!finished &&
        atomic_load_explicit(&fixture->speech_offset, memory_order_acquire) ==
            fixture->pcm_len) {
      rc = h2_gizclaw_service_audio_end(fixture->actors[H2_GIZCLAW_E2E_OWNER].service);
      if (rc == H2_PAL_OK)
        rc = h2_gizclaw_service_audio_end(fixture->actors[H2_GIZCLAW_E2E_OWNER].service);
      if (rc != H2_PAL_OK)
        return rc;
      finished = true;
    }
    rc = h2_gizclaw_req_wait(request, 10u);
    if (rc == H2_PAL_OK)
      return finished ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
    if (rc != H2_PAL_ERR_TIMEOUT)
      return rc;
    /* Also bound a provider that returns terminal TIMEOUT immediately. */
    rc = h2_pal_time_sleep_ms(fixture->time, 1u);
    if (rc != H2_PAL_OK)
      return rc;
  }
}

int h2_gizclaw_e2e_run_speech(h2_gizclaw_e2e_fixture_t *fixture,
                              h2_gizclaw_resp_storage_t *storage) {
  if (fixture == NULL || storage == NULL || fixture->pcm == NULL ||
      fixture->pcm_len == 0u || (fixture->pcm_len & 1u) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  if (service == NULL || fixture->speech_track != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  atomic_init(&fixture->speech_offset, 0u);
  const h2_gizclaw_pcm_track_config_t track_config = {
      .allocator = fixture->allocator,
      .uplink_capacity = 4096u, .downlink_capacity = 1024u};
  int rc = h2_gizclaw_pcm_track_create(&track_config, &fixture->speech_track);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_service_set_track(service, fixture->speech_track);
  if (rc != H2_PAL_OK) {
    (void)h2_gizclaw_pcm_track_destroy(&fixture->speech_track);
    return rc;
  }
  fixture->speech_track_bound = true;
  const h2_gizclaw_speech_transcribe_options_t transcribe_options = {
      .model_name = h2_gizclaw_e2e_str("asr"),
      .content_type = h2_gizclaw_e2e_str("audio/L16;rate=16000;channels=1"),
      .language = h2_gizclaw_e2e_str("en"),
  };

  const h2_gizclaw_speech_extract_options_t extract_options = {
      .asr_model_name = h2_gizclaw_e2e_str("asr"),
      .extract_model_name =
          h2_gizclaw_e2e_str("user-chat-with-assistant.extract"),
      .content_type = h2_gizclaw_e2e_str("audio/L16;rate=16000;channels=1"),
      .language = h2_gizclaw_e2e_str("en"),
      .schema_json = h2_gizclaw_e2e_str(
          "{\"type\":\"object\",\"properties\":{\"color\":{\"type\":\"string\","
          "\"enum\":[\"blue\"]}},\"required\":[\"color\"],"
          "\"additionalProperties\":false}"),
      .instruction = h2_gizclaw_e2e_str("Extract the spoken color marker."),
  };

  for (unsigned int kind = 0u; kind < 2u && rc == H2_PAL_OK; ++kind) {
    atomic_store_explicit(&fixture->speech_offset, 0u, memory_order_release);
    h2_gizclaw_req_t *request = NULL;
    const char *create_symbol = kind == 0u
                                    ? "h2_gizclaw_req_create_speech_transcribe"
                                    : "h2_gizclaw_req_create_speech_extract";
    const char *parse_symbol = kind == 0u
                                   ? "h2_gizclaw_resp_parse_speech_transcribe"
                                   : "h2_gizclaw_resp_parse_speech_extract";
    rc = kind == 0u ? h2_gizclaw_req_create_speech_transcribe(
                          service, 20u, &transcribe_options, 30000u, &request)
                    : h2_gizclaw_req_create_speech_extract(
                          service, 21u, &extract_options, 30000u, &request);
    h2_gizclaw_e2e_evidence(create_symbol, "speech", rc);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_service_audio_start(service);
    if (rc == H2_PAL_OK)
      rc = speech_wait(fixture, request);
    if (rc == H2_PAL_OK) {
      if (kind == 0u) {
        h2_gizclaw_speech_transcribe_response_t response = {0};
        rc = h2_gizclaw_resp_parse_speech_transcribe(request, storage,
                                                     &response);
        if (rc == H2_PAL_OK && !speech_view_has_blue(response.transcript))
          rc = H2_PAL_ERR_FORMAT;
      } else {
        h2_gizclaw_speech_extract_response_t response = {0};
        rc = h2_gizclaw_resp_parse_speech_extract(request, storage, &response);
        if (rc == H2_PAL_OK && !speech_view_has_blue(response.transcript))
          rc = H2_PAL_ERR_FORMAT;
        if (rc == H2_PAL_OK)
          rc = h2_gizclaw_e2e_validate_color_json(fixture->allocator,
                                                  response.result_json);
      }
      h2_gizclaw_e2e_evidence(parse_symbol, "speech", rc);
    }
    if (rc != H2_PAL_OK && request != NULL)
      (void)h2_gizclaw_req_cancel(request);
    h2_gizclaw_req_release(request);
    storage->used = 0u;
  }
  const int unset_rc =
      h2_gizclaw_service_unset_track(service, fixture->speech_track);
  if (unset_rc == H2_PAL_OK)
    fixture->speech_track_bound = false;
  if (unset_rc == H2_PAL_OK) {
    const int destroy_rc = h2_gizclaw_pcm_track_destroy(&fixture->speech_track);
    if (rc == H2_PAL_OK)
      rc = destroy_rc;
  }
  /* A failed unset retains the allocated Track until fixture teardown. */
  return rc == H2_PAL_OK ? unset_rc : rc;
}
