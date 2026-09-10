#include "h2_app_test_mem.h"
#include "h2_app_test_time.h"
#include "h2_gizclaw_e2e_speech.h"
#include "h2_gizclaw_pcm_track_fake.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum {
  NORMAL,
  CREATE_ERROR,
  DO_ERROR,
  REMOTE_ERROR,
  PARSE_ERROR,
  EMPTY_TRANSCRIPT,
  BAD_JSON,
  PREMATURE_SUCCESS,
  FINISH_ERROR,
  WAIT_TIMEOUT,
  SET_ERROR,
  UNSET_ERROR,
  EXTRACT_ERROR,
  SLEEP_ERROR,
  CLOCK_ERROR
};
static unsigned s_mode, s_created, s_released, s_finished, s_cancelled;
static unsigned s_unsets;
static int s_service;
static h2_gizclaw_track_t *s_track;
static uint8_t s_pcm[1346];

struct h2_gizclaw_req {
  bool extract, started, finished;
  size_t received;
};

static h2_app_test_mem_t test_mem;
static h2_app_test_time_t test_time;
static void *allocate(void *user, size_t len) {
  (void)user;
  return h2_pal_mem_alloc(&test_mem.api, len);
}
static void release(void *user, void *ptr) {
  (void)user;
  h2_pal_mem_free(&test_mem.api, ptr);
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t ms) {
  assert(fixture != NULL);
  return test_time.monotonic_ms + ms <= 31000u;
}
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *value) {
  return (h2_gizclaw_str_t){.data = value, .len = strlen(value)};
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && strcmp(stage, "speech") == 0);
  (void)rc;
}
h2_pal_result_t h2_gizclaw_service_set_track(h2_gizclaw_service_t *service,
                                             h2_gizclaw_track_t *track) {
  assert(service == (h2_gizclaw_service_t *)&s_service && s_track == NULL);
  if (s_mode == SET_ERROR)
    return H2_PAL_ERR_INVALID_STATE;
  s_track = track;
  return fake_pcm_track_bind(track);
}
h2_pal_result_t
h2_gizclaw_service_unset_track(h2_gizclaw_service_t *service,
                               h2_gizclaw_track_t *track) {
  assert(service == (h2_gizclaw_service_t *)&s_service && track == s_track);
  ++s_unsets;
  if (s_mode == UNSET_ERROR)
    return H2_PAL_ERR_IO;
  fake_pcm_track_unbind(track);
  s_track = NULL;
  return H2_PAL_OK;
}
static int create(h2_gizclaw_service_t *service, uint32_t timeout, bool extract,
                  h2_gizclaw_req_t **out) {
  assert(service == (h2_gizclaw_service_t *)&s_service && timeout == 30000u);
  *out = NULL;
  if (s_mode == CREATE_ERROR)
    return H2_PAL_ERR_NO_MEMORY;
  *out = allocate(NULL, sizeof(**out));
  assert(*out != NULL);
  **out = (h2_gizclaw_req_t){.extract = extract};
  ++s_created;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_create_speech_transcribe(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_transcribe_options_t *options, uint32_t timeout,
    h2_gizclaw_req_t **out) {
  assert(identity == 20u && options->content_type.len > 0u);
  return create(service, timeout, false, out);
}
h2_pal_result_t h2_gizclaw_req_create_speech_extract(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_extract_options_t *options, uint32_t timeout,
    h2_gizclaw_req_t **out) {
  assert(identity == 21u && options->schema_json.len > 0u);
  return create(service, timeout, true, out);
}
static h2_gizclaw_req_t *s_audio_request;
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(user == NULL && input_read == NULL && output_write == NULL &&
         !request->started);
  if (s_mode == DO_ERROR)
    return H2_PAL_ERR_NO_SPACE;
  request->started = true;
  s_audio_request = request;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request,
                                    uint32_t timeout) {
  assert(request->started && timeout == 10u && s_track != NULL);
  test_time.monotonic_ms += timeout;
  if (s_mode == REMOTE_ERROR || (s_mode == EXTRACT_ERROR && request->extract))
    return H2_GIZCLAW_ERR_REMOTE;
  if (s_mode == WAIT_TIMEOUT)
    return H2_PAL_ERR_TIMEOUT;
  if (s_mode == PREMATURE_SUCCESS)
    return H2_PAL_OK;
  if (request->finished && request->received == sizeof(s_pcm)) {
    return H2_PAL_OK;
  }
  uint8_t data[16];
  const size_t left = sizeof(s_pcm) - request->received;
  const size_t wanted = left < sizeof(data) ? left : sizeof(data);
  int rc = fake_pcm_track_service_read(s_track, data, wanted);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return H2_PAL_ERR_TIMEOUT;
  assert(rc == H2_PAL_OK);
  assert(wanted > 0u && (wanted & 1u) == 0u && wanted <= sizeof(data));
  assert(request->received + wanted <= sizeof(s_pcm));
  assert(memcmp(data, s_pcm + request->received, wanted) == 0);
  request->received += wanted;
  return H2_PAL_ERR_TIMEOUT;
}
h2_pal_result_t h2_gizclaw_service_audio_start(h2_gizclaw_service_t *service) {
  assert(service != NULL && s_audio_request != NULL);
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_service_audio_end(h2_gizclaw_service_t *service) {
  assert(service != NULL);
  h2_gizclaw_req_t *request = s_audio_request;
  assert(request->received + fake_pcm_track_uplink_pending(s_track) ==
         sizeof(s_pcm));
  ++s_finished;
  if (s_mode == FINISH_ERROR)
    return H2_PAL_ERR_IO;
  request->finished = true;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request) {
  assert(request != NULL);
  ++s_cancelled;
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *request) {
  if (request != NULL) {
    ++s_released;
    release(NULL, request);
  }
}
h2_pal_result_t h2_gizclaw_resp_parse_speech_transcribe(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_speech_transcribe_response_t *out) {
  assert(!request->extract && request->finished && storage != NULL);
  if (s_mode == PARSE_ERROR)
    return H2_PAL_ERR_NO_SPACE;
  out->transcript = h2_gizclaw_e2e_str(
      s_mode == EMPTY_TRANSCRIPT ? "" : "The color is BLUE.");
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_resp_parse_speech_extract(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_speech_extract_response_t *out) {
  assert(request->extract && request->finished && storage != NULL);
  out->transcript = h2_gizclaw_e2e_str("blue");
  out->result_json = h2_gizclaw_e2e_str(
      s_mode == BAD_JSON ? "{\"color\":\"blue\",\"extra\":true}"
                         : "{\"color\":\"blue\"}");
  return H2_PAL_OK;
}

static void run_case(unsigned mode, int expected) {
  s_mode = mode;
  test_time.read = (h2_app_test_fault_t){.result=H2_PAL_ERR_IO,
      .remaining=mode == CLOCK_ERROR ? UINT32_MAX : 0u};
  test_time.sleep = (h2_app_test_fault_t){.result=H2_PAL_ERR_IO,
      .remaining=mode == SLEEP_ERROR ? UINT32_MAX : 0u};
  test_time.monotonic_ms = s_created = s_released = s_finished = s_cancelled = s_unsets = 0u;
  assert(test_mem.live_blocks == 0u && s_track == NULL);
  h2_gizclaw_e2e_fixture_t *fixture = calloc(1u, sizeof(*fixture));
  assert(fixture != NULL);
  fixture->allocator = &test_mem.api;
  fixture->time = &test_time.api;
  fixture->pcm = s_pcm;
  fixture->pcm_len = sizeof(s_pcm);
  fixture->actors[0].service = (h2_gizclaw_service_t *)&s_service;
  uint8_t response[1024];
  h2_gizclaw_resp_storage_t storage = {.data = response,
                                       .capacity = sizeof(response)};
  assert(h2_gizclaw_e2e_run_speech(fixture, &storage) == expected);
  assert(s_created == s_released);
  assert(test_mem.live_blocks == (mode == UNSET_ERROR ? 3u : 0u));
  assert(s_unsets == (mode == SET_ERROR ? 0u : 1u));
  if (mode == NORMAL || mode == UNSET_ERROR) {
    assert(s_created == 2u && s_finished == 4u && s_cancelled == 0u);
  } else if (mode != CREATE_ERROR && mode != SET_ERROR) {
    assert(s_cancelled == 1u);
  }
  if (mode == UNSET_ERROR) {
    /* Returning from the case must not invalidate the still-borrowed Track. */
    assert(fixture->speech_track_bound && s_track == fixture->speech_track);
    uint8_t data[8];
    assert(fake_pcm_track_service_read(s_track, data, sizeof(data)) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_gizclaw_e2e_run_speech(fixture, &storage) ==
           H2_PAL_ERR_INVALID_STATE);
    fake_pcm_track_unbind(s_track);
    s_track = NULL; /* Mock Service teardown ends the borrow. */
    assert(h2_gizclaw_pcm_track_destroy(&fixture->speech_track) == H2_PAL_OK);
  } else {
    assert(!fixture->speech_track_bound && s_track == NULL);
  }
  free(fixture);
}

int main(void) {
  h2_app_test_mem_init(&test_mem, NULL);
  h2_app_test_time_init(&test_time, 0u);
  for (size_t i = 0u; i < sizeof(s_pcm); ++i)
    s_pcm[i] = (uint8_t)i;
  run_case(NORMAL, H2_PAL_OK);
  run_case(CREATE_ERROR, H2_PAL_ERR_NO_MEMORY);
  run_case(DO_ERROR, H2_PAL_ERR_NO_SPACE);
  run_case(REMOTE_ERROR, H2_GIZCLAW_ERR_REMOTE);
  run_case(PARSE_ERROR, H2_PAL_ERR_NO_SPACE);
  run_case(EMPTY_TRANSCRIPT, H2_PAL_ERR_FORMAT);
  run_case(BAD_JSON, H2_PAL_ERR_FORMAT);
  run_case(PREMATURE_SUCCESS, H2_PAL_ERR_INVALID_STATE);
  run_case(FINISH_ERROR, H2_PAL_ERR_IO);
  run_case(WAIT_TIMEOUT, H2_PAL_ERR_TIMEOUT);
  run_case(SET_ERROR, H2_PAL_ERR_INVALID_STATE);
  run_case(UNSET_ERROR, H2_PAL_ERR_IO);
  run_case(EXTRACT_ERROR, H2_GIZCLAW_ERR_REMOTE);
  run_case(SLEEP_ERROR, H2_PAL_ERR_IO);
  run_case(CLOCK_ERROR, H2_PAL_ERR_IO);
  const char *invalid[] = {"",
                           "blue",
                           "null",
                           "[]",
                           "{}",
                           "{\"color\":42}",
                           "{\"color\":null}",
                           "{\"color\":\"red\"}",
                           "{\"wrong\":\"blue\"}",
                           "{\"color\":\"blue\",\"extra\":1}",
                           "{\"color\":\"blue\",\"color\":\"blue\"}",
                           "{\"color\":\"blue\"} trailing",
                           "{\"color\":\"blue\",}",
                           "{\"color\":\"blue\\u0000\"}"};
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    assert(h2_gizclaw_e2e_validate_color_json(
               &test_mem.api, h2_gizclaw_e2e_str(invalid[i])) != H2_PAL_OK);
    assert(test_mem.live_blocks == 0u);
  }
  assert(h2_gizclaw_e2e_validate_color_json(
             &test_mem.api, h2_gizclaw_e2e_str(" { \"color\" : \"bl\\u0075e\" } ")) ==
         H2_PAL_OK);
  test_mem.calls = 0u;
  assert(h2_gizclaw_e2e_validate_color_json(
             &test_mem.api, h2_gizclaw_e2e_str("{\"color\":\"blue\"}")) == H2_PAL_OK);
  const unsigned allocations = test_mem.calls;
  for (unsigned i = 1u; i <= allocations; ++i) {
    test_mem.calls = 0u;
    test_mem.fail_at = i;
    assert(h2_gizclaw_e2e_validate_color_json(
               &test_mem.api, h2_gizclaw_e2e_str("{\"color\":\"blue\"}")) != H2_PAL_OK);
    assert(test_mem.live_blocks == 0u);
  }
  return 0;
}
