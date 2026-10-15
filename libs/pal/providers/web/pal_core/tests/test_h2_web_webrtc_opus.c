#include "h2_web_main_thread.h"
/*
 * An Opus vtable Track (the GizClaw model) over real browser WebRTC: tagged
 * packets leave through the encoded sender transform, the Pion fixture echoes
 * the RTP payloads, and the receiver transform hands them back to the Track.
 */
#include "h2_web_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
/* clang-format off */
EM_JS(void, answer_offer,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer", "u32"], "pointer",
    async (sdp, len) => {
  const response = await fetch('/pion/offer', {
    method: 'POST', body: UTF8ToString(sdp, len),
    signal: AbortSignal.timeout(15000)});
  if (!response.ok) throw new Error('Pion offer failed: ' + response.status);
  const text = await response.text();
  const size = lengthBytesUTF8(text) + 1;
  const answer = _malloc(size);
  stringToUTF8(text, answer, size);
  return answer;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, ice_url,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "pointer",
    () => {
  const size = lengthBytesUTF8(Module.iceURL) + 1;
  const url = _malloc(size);
  stringToUTF8(Module.iceURL, url, size);
  return url;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, transform_kind,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => {
  return typeof RTCRtpScriptTransform === 'function' ? 2 : 1;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, legacy_mode,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => { return Module.legacy ? 1 : 0; });
});
/* clang-format on */
typedef struct opus_track {
  _Atomic unsigned sent;
  _Atomic unsigned echoed;
  _Atomic unsigned foreign;
  _Atomic int calls_after_unset;
  _Atomic int unset;
} opus_track_t;

static opus_track_t s_track;
static const h2_pal_webrtc_api_t *s_api;
static h2_pal_webrtc_peer_t *s_peer;
static h2_pal_webrtc_track_t *s_binding;

/* Called while the media worker is paused in its main-thread bridge, before
 * it can borrow the Track again. This makes the detach overlap deterministic. */
EMSCRIPTEN_KEEPALIVE int web_test_opus_unset_in_bridge(void) {
  int rc = h2_pal_webrtc_peer_unset_track(s_api, s_peer, s_binding);
  if (rc == H2_PAL_OK) s_track.unset = 1;
  return rc;
}

/* clang-format off */
EM_JS(void, arm_bridge_unset,
    (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "i32"], null, (peer, rx) => {
    const original = h2WebMain;
    h2WebMain = (context, result, completion, types, returnType, callback) => {
      const relevant = returnType === 'i32' && types[0] === 'u32' &&
        types.length === (rx ? 3 : 1);
      original(context, result, completion, types, returnType, (...args) => {
        const value = callback(...args);
        if (relevant && args[0] === peer && (rx ? value >= 0 : value < 3)) {
          h2WebMain = original;
          const rc = Module['_web_test_opus_unset_in_bridge']();
          if (rc !== 0) throw Error('overlapping Track unset failed: ' + rc);
        }
        return value;
      });
    };
  });
});
/* clang-format on */

static h2_pal_result_t track_read(void *user, uint8_t *opus, size_t capacity,
                                  size_t *out_len) {
  opus_track_t *track = user;
  if (track->unset)
    ++track->calls_after_unset;
  // A CELT fullband 20 ms TOC byte followed by a recognizable tag.
  static const uint8_t tag[] = {0xf8u, 'H', '2', 'W', 'E', 'B'};
  if (capacity < sizeof(tag) + 2u)
    return H2_PAL_ERR_NO_SPACE;
  memcpy(opus, tag, sizeof(tag));
  opus[sizeof(tag)] = (uint8_t)track->sent;
  opus[sizeof(tag) + 1u] = (uint8_t)(track->sent >> 8u);
  *out_len = sizeof(tag) + 2u;
  ++track->sent;
  return H2_PAL_OK;
}

static h2_pal_result_t track_write(void *user, const uint8_t *opus,
                                   size_t opus_len) {
  opus_track_t *track = user;
  if (track->unset)
    ++track->calls_after_unset;
  if (opus_len == 8u && opus[0] == 0xf8u && memcmp(opus + 1, "H2WEB", 5u) == 0)
    ++track->echoed;
  else
    ++track->foreign;
  return H2_PAL_OK;
}

static const h2_pal_webrtc_track_vtable_t s_vtable = {
    .read = track_read,
    .write = track_write,
};

static void pump_for(h2_web_platform_t *platform,
                     const h2_pal_webrtc_api_t *api, h2_pal_webrtc_peer_t *peer,
                     double duration_ms, int (*done)(void)) {
  const double deadline = emscripten_get_now() + duration_ms;
  while (emscripten_get_now() < deadline && (done == NULL || !done())) {
    assert(h2_web_platform_pump(platform, 16u, NULL) == H2_PAL_OK);
    if (peer != NULL) {
      h2_pal_webrtc_event_t event = {0};
      while (h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK) {
        assert(event.kind != H2_PAL_WEBRTC_EVENT_ERROR);
        h2_pal_webrtc_event_release(&event);
      }
    }
    h2_web_worker_sleep(5u);
  }
}

static int echoed_enough(void) { return s_track.echoed >= 25u; }
static int unset_done(void) { return s_track.unset; }

static int run(int rx_overlap) {
  s_track.sent = s_track.echoed = s_track.foreign = 0u;
  s_track.calls_after_unset = s_track.unset = 0;
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  assert(platform != NULL);
  const h2_pal_webrtc_api_t *api = h2_web_platform_webrtc_api(platform);
  h2_pal_webrtc_peer_t *peer = NULL;
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  char *url = ((char *)h2_web_main_call(ice_url, NULL).ptr);
  const h2_pal_webrtc_ice_server_t ice = {
      .url = {.data = url, .len = strlen(url)}};
  assert(h2_pal_webrtc_peer_add_ice_server(api, peer, &ice) == H2_PAL_OK);
  free(url);

  h2_pal_webrtc_track_t track = {.user = &s_track, .vtable = &s_vtable};
  h2_pal_webrtc_track_t incomplete = {.user = &s_track};
  assert(h2_pal_webrtc_peer_set_track(api, peer, &incomplete) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_webrtc_peer_set_track(api, peer, &track) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_set_track(api, peer, &track) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
  h2_pal_webrtc_event_t event = {0};
  const double deadline = emscripten_get_now() + 10000.0;
  while (event.kind != H2_PAL_WEBRTC_EVENT_LOCAL_SDP) {
    assert(emscripten_get_now() < deadline);
    h2_pal_webrtc_event_release(&event);
    const h2_pal_result_t rc = h2_pal_webrtc_peer_poll(api, peer, 100, &event);
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_TIMEOUT);
  }
  char *answer =
      ((char *)h2_web_main_call(
           answer_offer, (const void *[]){&(const char *){event.sdp.data},
                                          &(size_t){event.sdp.len}})
           .ptr);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             api, peer, H2_PAL_WEBRTC_SDP_ANSWER,
             (h2_pal_webrtc_str_t){.data = answer, .len = strlen(answer)}) ==
         H2_PAL_OK);
  free(answer);

  pump_for(platform, api, peer, 15000.0, echoed_enough);
  printf("WEB_OPUS transform=%s sent=%u echoed=%u foreign=%u\n",
         ((int)h2_web_main_call(transform_kind, NULL).i32) == 2 ? "script" : "encoded-streams", s_track.sent,
         s_track.echoed, s_track.foreign);
  assert(s_track.echoed >= 25u);
  // The legacy run hides RTCRtpScriptTransform to force createEncodedStreams.
  assert(!((int)h2_web_main_call(legacy_mode, NULL).i32) ||
         ((int)h2_web_main_call(transform_kind, NULL).i32) == 1);

  // After unset the provider never calls the Track again.
  s_api = api;
  s_peer = peer;
  s_binding = &track;
  (void)h2_web_main_call(arm_bridge_unset,
      (const void *[]){&(uintptr_t){(uintptr_t)peer}, &rx_overlap});
  pump_for(platform, api, peer, 5000.0, unset_done);
  assert(s_track.unset);
  pump_for(platform, api, peer, 300.0, NULL);
  assert(s_track.calls_after_unset == 0);

  // Reattach is refused after the offer, as for browser-media Tracks.
  assert(h2_pal_webrtc_peer_set_track(api, peer, &track) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_pal_webrtc_peer_close(api, peer);
  h2_pal_result_t destroyed = H2_PAL_ERR_BUSY;
  for (int turn = 0; turn < 200 && destroyed == H2_PAL_ERR_BUSY; ++turn) {
    pump_for(platform, api, NULL, 5.0, NULL);
    destroyed = h2_web_platform_destroy(platform);
  }
  assert(destroyed == H2_PAL_OK);
  printf("WEB_OPUS bridge=%s unset=silent close=released PASS\n",
         rx_overlap ? "rx" : "tx");
  return 0;
}

int main(void) {
  assert(run(0) == 0);
  assert(run(1) == 0);
  puts("WEB_OPUS unset=silent close=released PASS");
  // Report now: browser timers may keep the runtime alive after main.
  /* clang-format off */
MAIN_THREAD_EM_ASM({ report(0); });
/* clang-format on */
  return 0;
}
