/*
 * Real-browser scenarios driven by run_browser_platform.py through the Chrome
 * DevTools Protocol. The page selects one scenario with location.hash and
 * reports each step to the harness as a console line. Every scenario runs in a
 * libco task while a second task keeps ticking, like a portable App.
 */
#include "h2_mp4_decoder.h"
#include "h2_web_fs.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

EM_JS_DEPS(browser_platform_test, "$stringToNewUTF8,$FS");

// clang-format off
EM_JS(void, test_report, (const char *step, int code, const char *detail), {
  const body = JSON.stringify({step: UTF8ToString(step), code,
                               detail: UTF8ToString(detail)});
  // The harness reads steps from the console, which works while offline.
  console.log(`STEP ${body}`);
});

EM_JS(char *, test_scenario, (), {
  return stringToNewUTF8(decodeURIComponent(location.hash.slice(1)));
});

EM_JS(char *, test_param, (const char *name), {
  const value = new URLSearchParams(location.search).get(UTF8ToString(name));
  return stringToNewUTF8(value || "");
});

// Chrome's CDP quota override does not bound IndexedDB writes, so quota
// exhaustion is injected where IDBFS stores records: the browser's own
// QuotaExceededError travels through the real IDBFS commit path.
EM_JS(void, test_inject_quota_exceeded, (), {
  const put = IDBObjectStore.prototype.put;
  IDBObjectStore.prototype.put = function(...args) {
    if (String(args[1] || "").includes("quota-")) {
      throw new DOMException("injected quota exhaustion", "QuotaExceededError");
    }
    return put.apply(this, args);
  };
});

EM_JS(void, test_seed_assets, (), {
  FS.mkdirTree('/assets/ui');
  FS.writeFile('/assets/ui/logo.txt', 'preloaded');
});
// clang-format on

typedef struct test_context {
  h2_web_platform_t *platform;
  const char *scenario;
  int result;
  int done;
  int ticks;
  int stop_ticker;
} test_context_t;

static test_context_t s_test;

#define STEP_CHECK(step, condition, detail)                                    \
  do {                                                                         \
    if (!(condition)) {                                                        \
      char message_[256];                                                      \
      (void)snprintf(message_, sizeof(message_), "%s (line %d)", detail,      \
                     __LINE__);                                                \
      test_report(step, 1, message_);                                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static void test_ticker(void *user) {
  test_context_t *context = user;
  while (!context->stop_ticker) {
    ++context->ticks;
    (void)h2_pal_time_sleep_ms(h2_web_platform_time_api(context->platform),
                               10u);
  }
}

/* ---------------------------------------------------------------- netif */

typedef struct netif_events {
  int count;
  h2_pal_netif_default_changed_t last;
} netif_events_t;

static int netif_record(void *user, const h2_pal_system_event_t *event) {
  netif_events_t *events = user;
  if (event->payload_size == sizeof(events->last)) {
    memcpy(&events->last, event->payload, sizeof(events->last));
    ++events->count;
  }
  return H2_PAL_OK;
}

static int wait_for_events(test_context_t *context, netif_events_t *events,
                           int count, uint32_t timeout_ms) {
  for (uint32_t waited = 0u; waited < timeout_ms && events->count < count;
       waited += 20u)
    (void)h2_pal_time_sleep_ms(h2_web_platform_time_api(context->platform),
                               20u);
  return events->count >= count;
}

static h2_pal_result_t http_get(test_context_t *context, const char *url,
                                int timeout_ms, h2_pal_http_response_t *out) {
  const h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = url, .len = strlen(url)},
      .timeout_ms = timeout_ms,
      .response_allocator = h2_web_platform_mem_api(),
  };
  h2_pal_http_response_reset(out);
  return (h2_pal_result_t)h2_pal_http_request(
      h2_web_platform_http_api(context->platform), &request, out);
}

static int run_netif(test_context_t *context) {
  const h2_pal_netif_api_t *netif = h2_web_platform_netif_api(context->platform);
  const h2_pal_system_event_api_t *events_api =
      h2_web_platform_system_event_api(context->platform);
  const h2_pal_netif_ref_t default_ref = h2_pal_netif_default_ref();
  h2_pal_netif_status_t status;
  STEP_CHECK("netif-online", h2_pal_netif_get_status(netif, &default_ref,
                                                     &status) == H2_PAL_OK &&
                                 h2_pal_netif_status_is_usable(&status) &&
                                 status.kind == H2_PAL_NETIF_KIND_HOST,
             "default path must be usable while online");
  netif_events_t events = {0};
  h2_pal_system_event_subscription_t *subscription = NULL;
  STEP_CHECK("netif-online",
             h2_pal_system_event_init(events_api) == H2_PAL_OK &&
                 h2_pal_system_event_subscribe(
                     events_api, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
                     netif_record, &events, &subscription) == H2_PAL_OK,
             "subscribe");
  test_report("netif-online", 0, "go offline");

  STEP_CHECK("netif-offline", wait_for_events(context, &events, 1, 10000u),
             "no default-changed event after going offline");
  STEP_CHECK("netif-offline",
             events.last.previous_valid == 1u && events.last.current_valid == 0u,
             "offline event must clear the default path");
  STEP_CHECK("netif-offline", h2_pal_netif_get_status(netif, &default_ref,
                                                      &status) ==
                                  H2_PAL_ERR_NOT_FOUND,
             "no default path while offline");
  h2_pal_http_response_t response;
  const h2_pal_result_t offline_rc =
      http_get(context, "/http/ok", 2000, &response);
  char detail[96];
  (void)snprintf(detail, sizeof(detail), "offline fetch rc=%d", offline_rc);
  STEP_CHECK("netif-offline", offline_rc == H2_PAL_ERR_UNAVAILABLE, detail);
  test_report("netif-offline", 0, "go online");

  STEP_CHECK("netif-restored", wait_for_events(context, &events, 2, 10000u),
             "no default-changed event after coming back online");
  STEP_CHECK("netif-restored",
             events.last.previous_valid == 0u && events.last.current_valid == 1u,
             "online event must restore the default path");
  const h2_pal_result_t online_rc =
      http_get(context, "/http/ok", 5000, &response);
  STEP_CHECK("netif-restored",
             online_rc == H2_PAL_OK && response.status_code == 200,
             "fetch must work again after recovery");
  h2_pal_http_response_free(h2_web_platform_http_api(context->platform),
                            &response);
  h2_pal_system_event_unsubscribe(events_api, subscription);
  h2_pal_system_event_deinit(events_api);
  test_report("netif-restored", 0, "ok");
  return 0;
}

/* ---------------------------------------------------------------- media */

typedef struct media_source {
  uint8_t *data;
  size_t size;
} media_source_t;

static h2_pal_result_t media_read_at(void *user, uint64_t offset, void *buffer,
                                     size_t capacity, size_t *out_read) {
  const media_source_t *media = user;
  if (offset > media->size)
    return H2_PAL_ERR_INVALID_ARG;
  size_t count = media->size - (size_t)offset;
  if (count > capacity)
    count = capacity;
  memcpy(buffer, media->data + (size_t)offset, count);
  *out_read = count;
  return H2_PAL_OK;
}

typedef struct media_pass {
  size_t frames;
  size_t pcm_samples;
  int64_t last_pts_us;
} media_pass_t;

/* Decode the whole file once, checking every frame against the contract. */
static h2_pal_result_t media_decode(h2_mp4_decoder_t *decoder,
                                    media_pass_t *pass, int16_t *pcm,
                                    size_t pcm_capacity) {
  *pass = (media_pass_t){.last_pts_us = -1};
  for (;;) {
    h2_mp4_decoder_frame_t *frame = NULL;
    h2_pal_result_t rc = h2_mp4_decoder_acquire_frame(decoder, 2000u, &frame);
    if (rc == H2_PAL_EXIT)
      return H2_PAL_OK;
    if (rc != H2_PAL_OK)
      return rc;
    h2_mp4_decoder_frame_info_t info;
    rc = h2_mp4_decoder_frame_get_info(decoder, frame, &info);
    if (rc == H2_PAL_OK &&
        (info.video_format != H2_VIDEO_PIXEL_FORMAT_RGB565 ||
         info.width != 160u || info.height != 96u ||
         info.video_plane_count != 1u ||
         info.video_planes[0].stride_bytes != 160u * 2u ||
         info.video_planes[0].bytes < 160u * 96u * 2u ||
         info.pts_us <= pass->last_pts_us))
      rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK) {
      pass->last_pts_us = info.pts_us;
      ++pass->frames;
      const size_t samples = info.pcm_samples_per_channel * info.pcm_channels;
      if (pcm != NULL && pass->pcm_samples + samples <= pcm_capacity)
        memcpy(pcm + pass->pcm_samples, info.pcm, samples * sizeof(*pcm));
      pass->pcm_samples += samples;
    }
    const h2_pal_result_t release = h2_mp4_decoder_release_frame(decoder, frame);
    if (rc != H2_PAL_OK)
      return rc;
    if (release != H2_PAL_OK)
      return release;
  }
}

static int run_display_and_speaker(test_context_t *context,
                                   const int16_t *pcm);

static int run_media(test_context_t *context) {
  h2_pal_http_response_t response;
  h2_pal_result_t rc =
      http_get(context, "/media/high_bframes_aac.mp4", 5000, &response);
  STEP_CHECK("media-decode", rc == H2_PAL_OK && response.status_code == 200,
             "fetch fixture");
  media_source_t source = {.data = response.body, .size = response.body_len};
  const h2_mp4_decoder_config_t config = {
      .allocator = h2_web_platform_mem_api(),
      .source = {.user = &source, .size = source.size,
                 .read_at = media_read_at},
      .video_decoder = *h2_web_platform_video_decoder_api(context->platform),
      .audio_decoder = *h2_web_platform_audio_decoder_api(context->platform),
      .video_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
      .require_video = 1,
      .require_audio = 1,
  };
  h2_mp4_decoder_t *decoder = NULL;
  rc = h2_mp4_decoder_open(&config, &decoder);
  char detail[160];
  static int16_t pcm[16000 * 3];
  if (rc == H2_PAL_ERR_UNSUPPORTED) {
    // Open-source Chromium builds ship without H.264/AAC. The provider must
    // say so instead of pretending to decode; the harness requires this
    // outcome only for such browsers.
    h2_pal_http_response_free(h2_web_platform_http_api(context->platform),
                              &response);
    test_report("media-decode", 0, "unsupported: no H.264/AAC WebCodecs");
    test_report("media-repeat", 0, "unsupported: no H.264/AAC WebCodecs");
    for (size_t index = 0u; index < 16000u; ++index)
      pcm[index] = (int16_t)((index % 40u) < 20u ? 4000 : -4000);
    return run_display_and_speaker(context, pcm);
  }
  (void)snprintf(detail, sizeof(detail), "open rc=%d", rc);
  STEP_CHECK("media-decode", rc == H2_PAL_OK, detail);
  media_pass_t first;
  const int ticks_before = context->ticks;
  rc = media_decode(decoder, &first, pcm, sizeof(pcm) / sizeof(pcm[0]));
  (void)snprintf(detail, sizeof(detail),
                 "pass1 rc=%d frames=%zu pcm=%zu ticks=%d", rc, first.frames,
                 first.pcm_samples, context->ticks - ticks_before);
  STEP_CHECK("media-decode",
             rc == H2_PAL_OK && first.frames >= 40u &&
                 first.pcm_samples >= 16000u,
             detail);
  STEP_CHECK("media-decode", context->ticks > ticks_before,
             "decoder waits must yield to other tasks");
  test_report("media-decode", 0, detail);

  // Repeat playback: reset rewinds to the start and decodes the same stream.
  media_pass_t second;
  rc = h2_mp4_decoder_reset(decoder);
  if (rc == H2_PAL_OK)
    rc = media_decode(decoder, &second, NULL, 0u);
  (void)snprintf(detail, sizeof(detail), "pass2 rc=%d frames=%zu pcm=%zu", rc,
                 second.frames, second.pcm_samples);
  STEP_CHECK("media-repeat",
             rc == H2_PAL_OK && second.frames == first.frames &&
                 second.pcm_samples == first.pcm_samples,
             detail);
  STEP_CHECK("media-repeat", h2_mp4_decoder_close(decoder) == H2_PAL_OK,
             "close");
  h2_pal_http_response_free(h2_web_platform_http_api(context->platform),
                            &response);
  test_report("media-repeat", 0, detail);
  return run_display_and_speaker(context, pcm);
}

static int run_display_and_speaker(test_context_t *context,
                                   const int16_t *pcm) {
  h2_pal_result_t rc = H2_PAL_OK;
  char detail[160];
  // Display: open is required first; draw and present reach the canvas.
  const h2_pal_display_api_t *display =
      h2_web_platform_display_api(context->platform);
  h2_display_info_t info;
  STEP_CHECK("display", h2_pal_display_get_info(display, &info) ==
                            H2_PAL_ERR_INVALID_STATE,
             "get_info before open");
  STEP_CHECK("display",
             h2_pal_display_open(display) == H2_PAL_OK &&
                 h2_pal_display_get_info(display, &info) == H2_PAL_OK &&
                 info.width == 240 && info.height == 240,
             "open 240x240");
  static uint16_t row[240 * 240];
  for (size_t index = 0u; index < 240u * 240u; ++index)
    row[index] = 0xf800u;
  const h2_display_rect_t rect = {0, 0, 240, 240};
  STEP_CHECK("display",
             h2_pal_display_draw_bitmap(display, &rect, row, 240u * 2u,
                                        H2_DISPLAY_PIXEL_RGB565) ==
                     H2_PAL_OK &&
                 h2_pal_display_present(display) == H2_PAL_OK,
             "draw and present");
  STEP_CHECK("display",
             h2_pal_display_close(display) == H2_PAL_OK &&
                 h2_pal_display_get_info(display, &info) ==
                     H2_PAL_ERR_INVALID_STATE &&
                 h2_pal_display_draw_bitmap(display, &rect, row, 240u * 2u,
                                            H2_DISPLAY_PIXEL_RGB565) ==
                     H2_PAL_ERR_INVALID_STATE &&
                 h2_pal_display_open(display) == H2_PAL_OK &&
                 h2_pal_display_close(display) == H2_PAL_OK,
             "closed display reports INVALID_STATE and reopens");
  test_report("display", 0, "open/draw/present/close/reopen");

  // Speaker: write is paced by the playback clock and drain waits for it.
  const h2_pal_audio_api_t *audio = h2_web_platform_audio_api(context->platform);
  const h2_audio_track_config_t track_config = {
      .format = {16000u, 320u, 1u, H2_AUDIO_SAMPLE_S16LE},
      .volume_factor_milli = 1000u,
  };
  h2_pal_audio_track_t *track = NULL;
  STEP_CHECK("speaker",
             h2_pal_audio_start_speaker(audio) == H2_PAL_OK &&
                 h2_pal_audio_create_track(audio, &track_config, &track) ==
                     H2_PAL_OK,
             "start speaker");
  const double started = emscripten_get_now();
  int would_block = 0;
  size_t written_samples = 0u;
  for (size_t offset = 0u; offset + 320u <= 16000u; offset += 320u) {
    h2_audio_frame_t frame = {
        .data = (void *)(pcm + offset),
        .capacity = 640u,
        .bytes = 640u,
        .sample_rate_hz = 16000u,
        .samples_per_channel = 320u,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    if (offset == 3200u &&
        h2_pal_audio_track_write(track, &frame, 0u) == H2_AUDIO_ERR_WOULD_BLOCK)
      would_block = 1;
    rc = (h2_pal_result_t)h2_pal_audio_track_write(track, &frame, 2000u);
    if (rc != H2_PAL_OK)
      break;
    written_samples += 320u;
  }
  const double write_ms = emscripten_get_now() - started;
  rc = rc == H2_PAL_OK
           ? (h2_pal_result_t)h2_pal_audio_track_drain(track, 3000u)
           : rc;
  const double total_ms = emscripten_get_now() - started;
  (void)snprintf(detail, sizeof(detail),
                 "rc=%d samples=%zu write_ms=%.0f total_ms=%.0f block=%d", rc,
                 written_samples, write_ms, total_ms, would_block);
  // One second of PCM: writes are throttled to the queue depth and drain
  // returns only once playback reached the end.
  STEP_CHECK("speaker",
             rc == H2_PAL_OK && written_samples == 16000u && would_block &&
                 write_ms >= 500.0 && total_ms >= 950.0 && total_ms < 2500.0,
             detail);
  STEP_CHECK("speaker",
             h2_pal_audio_track_close(track) == H2_PAL_OK &&
                 h2_pal_audio_stop_speaker(audio) == H2_PAL_OK,
             "close track");
  test_report("speaker", 0, detail);
  return 0;
}

/* ----------------------------------------------------------------- http */

typedef struct stream_sink {
  size_t total;
  size_t chunks;
  uint32_t checksum;
} stream_sink_t;

static int stream_read(void *user, const h2_pal_http_request_t *request,
                       const uint8_t *chunk, size_t chunk_len,
                       size_t total_read, size_t remaining) {
  (void)request;
  (void)remaining;
  stream_sink_t *sink = user;
  for (size_t index = 0u; index < chunk_len; ++index)
    sink->checksum = sink->checksum * 31u + chunk[index];
  sink->total = total_read;
  ++sink->chunks;
  return H2_PAL_OK;
}

typedef struct cancel_state {
  test_context_t *context;
  int after_ticks;
} cancel_state_t;

static int cancel_later(void *user) {
  cancel_state_t *state = user;
  return state->context->ticks >= state->after_ticks;
}

static int run_http(test_context_t *context) {
  const h2_pal_http_api_t *http = h2_web_platform_http_api(context->platform);
  h2_pal_http_response_t response;

  // 4xx keeps its status and body; it is not an transport error.
  h2_pal_result_t rc = http_get(context, "/http/status/404", 3000, &response);
  STEP_CHECK("http-status", rc == H2_PAL_OK && response.status_code == 404 &&
                                response.body_len == 7u &&
                                memcmp(response.body, "missing", 7u) == 0,
             "404 must return status and body");
  h2_pal_http_response_free(http, &response);
  test_report("http-status", 0, "404 body delivered");

  // POST body and custom headers survive the CORS preflight.
  static const char payload[] = "h2-web-post";
  const h2_pal_http_header_t headers[] = {
      {{"content-type", 12u}, {"application/octet-stream", 24u}},
      {{"x-h2-test", 9u}, {"yes", 3u}},
  };
  char url[160];
  char *cross = test_param("cross");
  (void)snprintf(url, sizeof(url), "%s/http/echo", cross);
  const h2_pal_http_request_t post = {
      .method = H2_PAL_HTTP_POST,
      .url = {.data = url, .len = strlen(url)},
      .headers = headers,
      .header_count = 2u,
      .body = (const uint8_t *)payload,
      .body_len = sizeof(payload) - 1u,
      .timeout_ms = 3000,
      .response_allocator = h2_web_platform_mem_api(),
  };
  rc = (h2_pal_result_t)h2_pal_http_request(http, &post, &response);
  STEP_CHECK("http-post", rc == H2_PAL_OK && response.status_code == 200 &&
                              response.body_len == sizeof(payload) - 1u + 4u &&
                              memcmp(response.body, "yes:", 4u) == 0 &&
                              memcmp(response.body + 4, payload,
                                     sizeof(payload) - 1u) == 0,
             "cross-origin POST with preflight must echo header and body");
  h2_pal_http_response_free(http, &response);
  test_report("http-post", 0, "cross-origin preflighted POST ok");

  // A cross-origin response without Access-Control-Allow-Origin is a CORS
  // failure the page cannot read; it must not look like success.
  (void)snprintf(url, sizeof(url), "%s/http/nocors", cross);
  rc = http_get(context, url, 3000, &response);
  STEP_CHECK("http-cors", rc == H2_PAL_ERR_IO && response.body == NULL,
             "CORS rejection must return IO");
  test_report("http-cors", 0, "CORS rejection is IO and logged");

  // Streaming: a large body arrives in several read_cb chunks.
  stream_sink_t sink = {0};
  uint8_t chunk[4096];
  const char *stream_url = "/http/stream";
  const h2_pal_http_request_t stream = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = stream_url, .len = strlen(stream_url)},
      .timeout_ms = 5000,
      .chunk_buf = chunk,
      .chunk_buf_cap = sizeof(chunk),
      .read_cb = stream_read,
      .user = &sink,
  };
  rc = (h2_pal_result_t)h2_pal_http_request(http, &stream, &response);
  (void)snprintf(url, sizeof(url), "stream rc=%d total=%zu chunks=%zu", rc,
                 sink.total, sink.chunks);
  STEP_CHECK("http-stream", rc == H2_PAL_OK && sink.total == 262144u &&
                                sink.chunks >= 64u,
             url);
  test_report("http-stream", 0, url);

  // Timeout aborts the fetch while the other task keeps running.
  const int ticks_before = context->ticks;
  rc = http_get(context, "/http/slow", 300, &response);
  STEP_CHECK("http-timeout", rc == H2_PAL_ERR_TIMEOUT,
             "slow response must time out");
  STEP_CHECK("http-timeout", context->ticks - ticks_before >= 10,
             "another task must keep running during the fetch");
  test_report("http-timeout", 0, "timeout without freezing tasks");

  // cancel_cb aborts an in-flight request promptly.
  cancel_state_t cancel = {.context = context,
                           .after_ticks = context->ticks + 10};
  const h2_pal_http_request_t cancelled = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = "/http/slow", .len = 10u},
      .timeout_ms = 5000,
      .response_allocator = h2_web_platform_mem_api(),
      .cancel_cb = cancel_later,
      .cancel_user = &cancel,
  };
  const double started = emscripten_get_now();
  rc = (h2_pal_result_t)h2_pal_http_request(http, &cancelled, &response);
  STEP_CHECK("http-cancel",
             rc == H2_PAL_ERR_CLOSED && emscripten_get_now() - started < 1500.0,
             "cancel must abort the fetch well before it completes");
  test_report("http-cancel", 0, "cancel aborts in flight");

  // response_buf overflow is NO_SPACE, like CoreHTTP.
  uint8_t small[8];
  const h2_pal_http_request_t overflow = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = stream_url, .len = strlen(stream_url)},
      .timeout_ms = 5000,
      .response_buf = small,
      .response_buf_cap = sizeof(small),
  };
  rc = (h2_pal_result_t)h2_pal_http_request(http, &overflow, &response);
  STEP_CHECK("http-overflow", rc == H2_PAL_ERR_NO_SPACE,
             "response_buf overflow must be NO_SPACE");
  test_report("http-overflow", 0, "NO_SPACE");
  free(cross);
  return 0;
}

/* ------------------------------------------------------------------- fs */

static const char *const k_readonly[] = {"/assets"};

static h2_pal_result_t fs_open(test_context_t *context, uint32_t lock_ms,
                               h2_web_fs_t **out_fs) {
  test_seed_assets();
  const h2_web_fs_config_t config = {
      .persistent_root = "/persist",
      .readonly_roots = k_readonly,
      .readonly_root_count = 1u,
      .lock_timeout_ms = lock_ms,
  };
  return h2_web_fs_open(context->platform, &config, out_fs);
}

static h2_pal_result_t fs_write_atomic(const h2_pal_fs_api_t *fs,
                                       const char *path, const char *text) {
  h2_pal_result_t rc = (h2_pal_result_t)h2_pal_fs_mkdir(fs, "/persist/app");
  h2_pal_fs_file_t *file = NULL;
  if (rc == H2_PAL_OK)
    rc = (h2_pal_result_t)h2_pal_fs_open(fs, "/persist/app/state.tmp",
                                         H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
  size_t written = 0u;
  if (rc == H2_PAL_OK)
    rc = (h2_pal_result_t)h2_pal_fs_write(fs, file, text, strlen(text),
                                          &written);
  if (rc == H2_PAL_OK)
    rc = (h2_pal_result_t)h2_pal_fs_sync(fs, file);
  if (file != NULL) {
    const h2_pal_result_t close_rc = (h2_pal_result_t)h2_pal_fs_close(fs, file);
    if (rc == H2_PAL_OK)
      rc = close_rc;
  }
  if (rc == H2_PAL_OK)
    rc = (h2_pal_result_t)h2_pal_fs_rename(fs, "/persist/app/state.tmp", path);
  return rc;
}

static int fs_read_text(const h2_pal_fs_api_t *fs, const char *path,
                        char *out, size_t out_size) {
  h2_pal_fs_file_t *file = NULL;
  if (h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_READ, &file) != H2_PAL_OK)
    return 0;
  size_t read = 0u;
  const int rc = h2_pal_fs_read(fs, file, out, out_size - 1u, &read);
  (void)h2_pal_fs_close(fs, file);
  out[rc == H2_PAL_OK ? read : 0u] = '\0';
  return rc == H2_PAL_OK;
}

static int run_fs(test_context_t *context, const char *scenario) {
  h2_web_fs_t *web_fs = NULL;
  if (strcmp(scenario, "fs-busy") == 0) {
    const h2_pal_result_t rc = fs_open(context, 500u, &web_fs);
    char detail[64];
    (void)snprintf(detail, sizeof(detail), "second tab open rc=%d", rc);
    STEP_CHECK(scenario, rc == H2_PAL_ERR_BUSY && web_fs == NULL, detail);
    test_report(scenario, 0, detail);
    return 0;
  }
  h2_pal_result_t rc = fs_open(context, 0u, &web_fs);
  char detail[160];
  (void)snprintf(detail, sizeof(detail), "open rc=%d", rc);
  STEP_CHECK(scenario, rc == H2_PAL_OK, detail);
  const h2_pal_fs_api_t *fs = h2_web_fs_api(web_fs);
  char text[64];

  if (strcmp(scenario, "fs-write") == 0) {
    rc = fs_write_atomic(fs, "/persist/app/state.txt", "generation-1");
    STEP_CHECK(scenario, rc == H2_PAL_OK, "atomic write");
    h2_web_fs_status_t status;
    STEP_CHECK(scenario,
               h2_web_fs_get_status(web_fs, &status) == H2_PAL_OK &&
                   status.committed_generation == status.changed_generation &&
                   status.last_result == H2_PAL_OK,
               "barriers must leave nothing pending");
    h2_pal_fs_file_t *file = NULL;
    STEP_CHECK(scenario,
               h2_pal_fs_open(fs, "/assets/ui/new.txt",
                              H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                              &file) == H2_PAL_ERR_UNSUPPORTED &&
                   h2_pal_fs_remove(fs, "/assets/ui/logo.txt") ==
                       H2_PAL_ERR_UNSUPPORTED,
               "read-only roots reject mutation");
    STEP_CHECK(scenario,
               fs_read_text(fs, "/assets/ui/logo.txt", text, sizeof(text)) &&
                   strcmp(text, "preloaded") == 0,
               "read-only roots stay readable");
    STEP_CHECK(scenario,
               h2_pal_fs_mkdir(fs, "/elsewhere") == H2_PAL_ERR_NOT_FOUND,
               "paths outside the roots are not exposed");
    // No close: the reload must find data committed by the barriers alone.
    test_report(scenario, 0, "written; reload without shutdown");
    for (;;)
      (void)h2_pal_time_sleep_ms(h2_web_platform_time_api(context->platform),
                                 1000u);
  }
  if (strcmp(scenario, "fs-verify") == 0) {
    STEP_CHECK(scenario,
               fs_read_text(fs, "/persist/app/state.txt", text, sizeof(text)) &&
                   strcmp(text, "generation-1") == 0,
               "data must survive the reload");
    h2_pal_fs_stat_t stat = {0};
    STEP_CHECK(scenario,
               h2_pal_fs_stat(fs, "/persist/app/state.tmp", &stat) ==
                   H2_PAL_ERR_NOT_FOUND,
               "the rename must be durable too");
    STEP_CHECK(scenario,
               h2_web_platform_destroy(context->platform) == H2_PAL_ERR_BUSY,
               "an open filesystem pins the platform");
    STEP_CHECK(scenario, h2_web_fs_close(web_fs) == H2_PAL_OK, "close");
    test_report(scenario, 0, "restored generation-1");
    return 0;
  }
  if (strcmp(scenario, "fs-hold") == 0) {
    test_report(scenario, 0, "holding the root");
    for (;;)
      (void)h2_pal_time_sleep_ms(h2_web_platform_time_api(context->platform),
                                 1000u);
  }
  if (strcmp(scenario, "fs-clear") == 0) {
    STEP_CHECK(scenario, h2_web_fs_clear(web_fs) == H2_PAL_OK, "clear");
    STEP_CHECK(scenario, h2_web_fs_close(web_fs) == H2_PAL_OK, "close");
    test_report(scenario, 0, "cleared");
    return 0;
  }
  if (strcmp(scenario, "fs-empty") == 0) {
    h2_pal_fs_stat_t stat = {0};
    STEP_CHECK(scenario,
               h2_pal_fs_stat(fs, "/persist/app/state.txt", &stat) ==
                   H2_PAL_ERR_NOT_FOUND,
               "cleared data must stay deleted after reload");
    STEP_CHECK(scenario, h2_web_fs_close(web_fs) == H2_PAL_OK, "close");
    test_report(scenario, 0, "empty after reload");
    return 0;
  }
  if (strcmp(scenario, "fs-quota") == 0) {
    // Every commit that stores a quota-* record hits the quota error.
    test_inject_quota_exceeded();
    static char block[16384];
    memset(block, 'q', sizeof(block) - 1u);
    rc = (h2_pal_result_t)h2_pal_fs_mkdir(fs, "/persist/app");
    for (int index = 0; index < 2 && rc == H2_PAL_OK; ++index) {
      char path[64];
      (void)snprintf(path, sizeof(path), "/persist/app/quota-%02d.bin", index);
      h2_pal_fs_file_t *file = NULL;
      rc = (h2_pal_result_t)h2_pal_fs_open(fs, path,
                                           H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
      size_t written = 0u;
      if (rc == H2_PAL_OK)
        rc = (h2_pal_result_t)h2_pal_fs_write(fs, file, block,
                                              sizeof(block) - 1u, &written);
      if (file != NULL) {
        const h2_pal_result_t close_rc =
            (h2_pal_result_t)h2_pal_fs_close(fs, file);
        if (rc == H2_PAL_OK)
          rc = close_rc;
      }
    }
    h2_web_fs_status_t status;
    (void)h2_web_fs_get_status(web_fs, &status);
    (void)snprintf(detail, sizeof(detail), "rc=%d last=%d error=%s", rc,
                   status.last_result, status.last_error);
    STEP_CHECK(scenario, rc == H2_PAL_ERR_NO_SPACE &&
                             status.last_result == H2_PAL_ERR_NO_SPACE &&
                             status.last_error[0] != '\0',
               detail);
    (void)h2_web_fs_close(web_fs);
    test_report(scenario, 0, detail);
    return 0;
  }
  test_report(scenario, 1, "unknown fs scenario");
  return 1;
}

static void run_scenario(void *user) {
  test_context_t *context = user;
  if (strcmp(context->scenario, "netif") == 0)
    context->result = run_netif(context);
  else if (strcmp(context->scenario, "http") == 0)
    context->result = run_http(context);
  else if (strcmp(context->scenario, "media") == 0)
    context->result = run_media(context);
  else if (strncmp(context->scenario, "fs-", 3u) == 0)
    context->result = run_fs(context, context->scenario);
  else {
    test_report(context->scenario, 1, "unknown scenario");
    context->result = 1;
  }
  context->stop_ticker = 1;
  context->done = 1;
}

int main(void) {
  const h2_web_platform_config_t config = {.display_width = 240,
                                           .display_height = 240};
  s_test.platform = h2_web_platform_create(&config);
  s_test.scenario = test_scenario();
  if (s_test.platform == NULL) {
    test_report(s_test.scenario, 1, "platform create");
    return 1;
  }
  const h2_pal_task_api_t *tasks = h2_web_platform_task_api(s_test.platform);
  h2_pal_task_t *ticker = NULL;
  h2_pal_task_t *runner = NULL;
  if (h2_pal_task_start(tasks, NULL, test_ticker, &s_test, &ticker) !=
          H2_PAL_OK ||
      h2_pal_task_start(tasks, NULL, run_scenario, &s_test, &runner) !=
          H2_PAL_OK) {
    test_report(s_test.scenario, 1, "task start");
    return 1;
  }
  while (!s_test.done) {
    (void)h2_web_platform_pump(s_test.platform, 16u, NULL);
    emscripten_sleep(1u);
  }
  h2_pal_task_t *const joined[] = {ticker, runner};
  for (size_t index = 0u; index < 2u; ++index) {
    while (h2_pal_task_join(tasks, joined[index]) == H2_PAL_ERR_BUSY) {
      (void)h2_web_platform_pump(s_test.platform, 16u, NULL);
      emscripten_sleep(1u);
    }
  }
  const h2_pal_result_t destroyed = h2_web_platform_destroy(s_test.platform);
  test_report("destroy", destroyed == H2_PAL_OK ? 0 : 1,
              destroyed == H2_PAL_OK ? "platform released" : "destroy busy");
  return s_test.result;
}
