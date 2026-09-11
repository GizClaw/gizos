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
#include <stdlib.h>
#include <string.h>

// clang-format off
EM_ASYNC_JS(char *, answer_offer, (const char *sdp, size_t len), {
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

EM_JS(char *, ice_url, (), {
  const size = lengthBytesUTF8(Module.iceURL) + 1;
  const url = _malloc(size);
  stringToUTF8(Module.iceURL, url, size);
  return url;
});

EM_JS(int, transform_kind, (), {
  return typeof RTCRtpScriptTransform === 'function' ? 2 : 1;
});
// clang-format on

typedef struct opus_track {
  unsigned sent;
  unsigned echoed;
  unsigned foreign;
  int calls_after_unset;
  int unset;
} opus_track_t;

static opus_track_t s_track;

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
    emscripten_sleep(5u);
  }
}

static int echoed_enough(void) { return s_track.echoed >= 25u; }

int main(void) {
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  assert(platform != NULL);
  const h2_pal_webrtc_api_t *api = h2_web_platform_webrtc_api(platform);
  h2_pal_webrtc_peer_t *peer = NULL;
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  char *url = ice_url();
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
  char *answer = answer_offer(event.sdp.data, event.sdp.len);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             api, peer, H2_PAL_WEBRTC_SDP_ANSWER,
             (h2_pal_webrtc_str_t){.data = answer, .len = strlen(answer)}) ==
         H2_PAL_OK);
  free(answer);

  pump_for(platform, api, peer, 15000.0, echoed_enough);
  printf("WEB_OPUS transform=%s sent=%u echoed=%u foreign=%u\n",
         transform_kind() == 2 ? "script" : "encoded-streams", s_track.sent,
         s_track.echoed, s_track.foreign);
  assert(s_track.echoed >= 25u);

  // After unset the provider never calls the Track again.
  assert(h2_pal_webrtc_peer_unset_track(api, peer, &track) == H2_PAL_OK);
  s_track.unset = 1;
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
  puts("WEB_OPUS unset=silent close=released PASS");
  // Report now: browser timers may keep the runtime alive after main.
  EM_ASM({ report(0); });
  return 0;
}
