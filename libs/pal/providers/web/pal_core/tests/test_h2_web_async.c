/*
 * Browser Promise waits must yield from libco tasks: while one task waits for
 * fetch, WebRTC or storage, another task keeps running. Node with fakes.
 */
#include "h2_web_fs.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "Web async test failed at line %d: %s\n", __LINE__,     \
              #condition);                                                     \
      emscripten_force_exit(1);                                                \
    }                                                                          \
  } while (0)

// clang-format off
EM_JS(void, test_install_fetch, (int delay_ms), {
  globalThis.fetch = (url, options) => new Promise((resolve, reject) => {
    const timer = setTimeout(() => resolve(new Response('teapot body', {
      status: 418, headers: {'x-test': 'yes'}})), delay_ms);
    options.signal.addEventListener('abort', () => {
      clearTimeout(timer);
      reject(new DOMException('aborted', 'AbortError'));
    });
  });
});

EM_JS(void, test_set_storage, (int indexed_db, int locks), {
  if (indexed_db) globalThis.indexedDB = {};
  else delete globalThis.indexedDB;
  const navigator = globalThis.navigator || {};
  if (locks) navigator.locks = {request() { return new Promise(() => {}); }};
  else delete navigator.locks;
});
// clang-format on

EM_JS(void, test_hang_create_offer, (int hang), {
  const prototype = globalThis.RTCPeerConnection.prototype;
  prototype.h2OriginalCreateOffer ||= prototype.createOffer;
  // pc.close() leaves pending WebRTC Promises unsettled.
  prototype.createOffer = hang ? () => new Promise(() => {})
                               : prototype.h2OriginalCreateOffer;
});

typedef struct test_state {
  h2_web_platform_t *platform;
  int ticks;
  int stop;
  int done;
  h2_pal_result_t http_result;
  h2_pal_webrtc_peer_t *offer_peer;
  h2_pal_result_t offer_result;
  int offer_done;
} test_state_t;

static test_state_t s_state;

static void offer_task(void *user) {
  test_state_t *state = user;
  state->offer_result = h2_pal_webrtc_peer_start_offer(
      h2_web_platform_webrtc_api(state->platform), state->offer_peer);
  state->offer_done = 1;
}

static void ticker(void *user) {
  test_state_t *state = user;
  while (!state->stop) {
    ++state->ticks;
    (void)h2_pal_time_sleep_ms(h2_web_platform_time_api(state->platform), 5u);
  }
}

static void run(void *user) {
  test_state_t *state = user;
  const h2_pal_http_api_t *http = h2_web_platform_http_api(state->platform);

  // Fetch from a task: the ticker advances while the response is pending, and
  // a 4xx status is delivered with its body.
  test_install_fetch(100);
  const h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = "https://example.test/x", .len = 22u},
      .timeout_ms = 2000,
      .response_allocator = h2_web_platform_mem_api(),
  };
  h2_pal_http_response_t response;
  h2_pal_http_response_reset(&response);
  int ticks = state->ticks;
  state->http_result =
      (h2_pal_result_t)h2_pal_http_request(http, &request, &response);
  CHECK(state->http_result == H2_PAL_OK);
  CHECK(response.status_code == 418 && response.body_len == 11u &&
        memcmp(response.body, "teapot body", 11u) == 0);
  CHECK(state->ticks - ticks >= 5);
  h2_pal_http_response_free(http, &response);

  // A timeout aborts the pending fetch without freezing the ticker.
  test_install_fetch(10000);
  h2_pal_http_request_t slow = request;
  slow.timeout_ms = 50;
  ticks = state->ticks;
  CHECK(h2_pal_http_request(http, &slow, &response) == H2_PAL_ERR_TIMEOUT);
  CHECK(state->ticks - ticks >= 3);

  // WebRTC poll waits yield too and end with TIMEOUT.
  const h2_pal_webrtc_api_t *webrtc =
      h2_web_platform_webrtc_api(state->platform);
  h2_pal_webrtc_peer_t *peer = NULL;
  CHECK(h2_pal_webrtc_peer_create(webrtc, &peer) == H2_PAL_OK);
  h2_pal_webrtc_event_t event = {0};
  ticks = state->ticks;
  CHECK(h2_pal_webrtc_peer_poll(webrtc, peer, 60, &event) ==
        H2_PAL_ERR_TIMEOUT);
  CHECK(state->ticks - ticks >= 5);
  h2_pal_webrtc_peer_close(webrtc, peer);

  // Closing a peer ends an offer whose browser Promise will never settle.
  test_hang_create_offer(1);
  CHECK(h2_pal_webrtc_peer_create(webrtc, &state->offer_peer) == H2_PAL_OK);
  h2_pal_task_t *offer = NULL;
  CHECK(h2_pal_task_start(h2_web_platform_task_api(state->platform), NULL,
                          offer_task, state, &offer) == H2_PAL_OK);
  (void)h2_pal_time_sleep_ms(h2_web_platform_time_api(state->platform), 20u);
  CHECK(!state->offer_done);
  h2_pal_webrtc_peer_close(webrtc, state->offer_peer);
  CHECK(h2_pal_task_join(h2_web_platform_task_api(state->platform), offer) ==
        H2_PAL_OK);
  CHECK(state->offer_done && state->offer_result == H2_PAL_ERR_CLOSED);
  test_hang_create_offer(0);

  // Persistent storage reports why it cannot open instead of pretending.
  h2_web_fs_t *fs = NULL;
  const h2_web_fs_config_t config = {.persistent_root = "/persist"};
  test_set_storage(0, 1);
  CHECK(h2_web_fs_open(state->platform, &config, &fs) ==
        H2_PAL_ERR_UNSUPPORTED);
  test_set_storage(1, 0);
  CHECK(h2_web_fs_open(state->platform, &config, &fs) ==
        H2_PAL_ERR_UNSUPPORTED);
  CHECK(fs == NULL);
  const char *const overlapping[] = {"/persist/assets"};
  const h2_web_fs_config_t invalid[] = {
      {.persistent_root = "relative"},
      {.persistent_root = "/"},
      {.persistent_root = "/persist/"},
      {.persistent_root = "/a/../b"},
      {.persistent_root = "/persist", .readonly_roots = overlapping,
       .readonly_root_count = 1u},
  };
  for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]);
       ++index)
    CHECK(h2_web_fs_open(state->platform, &invalid[index], &fs) ==
          H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_web_fs_close(NULL) == H2_PAL_OK);

  // A speaker track pins the platform until it is closed.
  const h2_pal_audio_api_t *audio = h2_web_platform_audio_api(state->platform);
  const h2_audio_track_config_t track_config = {
      .format = {16000u, 320u, 1u, H2_AUDIO_SAMPLE_S16LE},
      .volume_factor_milli = 1000u,
  };
  h2_pal_audio_track_t *track = NULL;
  CHECK(h2_pal_audio_create_track(audio, &track_config, &track) == H2_PAL_OK);
  CHECK(h2_web_platform_destroy(state->platform) == H2_PAL_ERR_BUSY);
  CHECK(h2_pal_audio_track_close(track) == H2_PAL_OK);

  state->stop = 1;
  state->done = 1;
}

int main(void) {
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  s_state.platform = h2_web_platform_create(&config);
  CHECK(s_state.platform != NULL);
  const h2_pal_task_api_t *tasks = h2_web_platform_task_api(s_state.platform);
  h2_pal_task_t *ticker_task = NULL;
  h2_pal_task_t *run_task = NULL;
  CHECK(h2_pal_task_start(tasks, NULL, ticker, &s_state, &ticker_task) ==
        H2_PAL_OK);
  CHECK(h2_pal_task_start(tasks, NULL, run, &s_state, &run_task) == H2_PAL_OK);
  // Destroy refuses while tasks are alive.
  CHECK(h2_web_platform_destroy(s_state.platform) == H2_PAL_ERR_BUSY);
  while (!s_state.done) {
    CHECK(h2_web_platform_pump(s_state.platform, 16u, NULL) == H2_PAL_OK);
    emscripten_sleep(1u);
  }
  h2_pal_task_t *const joined[] = {ticker_task, run_task};
  for (size_t index = 0u; index < 2u; ++index) {
    while (h2_pal_task_join(tasks, joined[index]) == H2_PAL_ERR_BUSY) {
      CHECK(h2_web_platform_pump(s_state.platform, 16u, NULL) == H2_PAL_OK);
      emscripten_sleep(1u);
    }
  }
  CHECK(h2_web_platform_destroy(s_state.platform) == H2_PAL_OK);
  printf("WEB_ASYNC PASS\n");
  return 0;
}
