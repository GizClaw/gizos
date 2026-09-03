#include "h2_web_platform.h"

// Test operations and checks must run in optimized builds too.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Embedded JS operators are not understood by the C formatter.
// clang-format off

static h2_web_platform_t *platform;
static const h2_pal_webrtc_api_t *api;
static h2_pal_webrtc_peer_t *peer;
static int observed;

EMSCRIPTEN_KEEPALIVE void web_test_destroy_while_waiting(void) {
  h2_web_platform_destroy(platform);
  observed =
      EM_ASM_INT({ return Module.h2WebRtcPeers.has($0); }, (uintptr_t)peer);
}

EMSCRIPTEN_KEEPALIVE void web_test_close_while_waiting(void) {
  h2_pal_webrtc_peer_close(api, peer);
  peer = NULL;
  observed++;
}

EMSCRIPTEN_KEEPALIVE void web_test_poll_while_waiting(void) {
  h2_pal_webrtc_event_t event = {0};
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_ERR_BUSY);
}

static void create_peer(void) {
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
}

static void drain(void) {
  h2_pal_webrtc_event_t event = {0};
  h2_pal_result_t rc;
  while ((rc = h2_pal_webrtc_peer_poll(api, peer, 0, &event)) == H2_PAL_OK)
    h2_pal_webrtc_event_release(&event);
  assert(rc == H2_PAL_ERR_WOULD_BLOCK);
}

static h2_pal_webrtc_channel_t *create_open_channel(void) {
  create_peer();
  const h2_pal_webrtc_channel_config_t config = {
      .label = {.data = "q", .len = 1}, .ordered = 1, .reliable = 1};
  h2_pal_webrtc_channel_t *channel = NULL;
  assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &config, &channel) ==
         H2_PAL_OK);
  EM_ASM({ Module.h2WebRtcChannels.get($0).dc.open(); }, (uintptr_t)channel);
  drain();
  return channel;
}

static void test_events(void) {
  h2_pal_webrtc_channel_t *channel = create_open_channel();
  h2_pal_webrtc_event_t event = {0};
  assert(h2_pal_webrtc_peer_poll(api, peer, -1, &event) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_webrtc_peer_poll(api, peer, 1, &event) == H2_PAL_ERR_TIMEOUT);
  EM_ASM(
      {
        setTimeout(() =>
                        {
                          Module._web_test_poll_while_waiting();
                          Module.h2WebRtcChannels.get($0).dc.onmessage(
                              {data : 'wake'});
                        },
                   2);
      },
      (uintptr_t)channel);
  double start = emscripten_get_now();
  assert(h2_pal_webrtc_peer_poll(api, peer, 1000, &event) == H2_PAL_OK);
  assert(emscripten_get_now() - start < 750);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE && event.is_text);
  assert(event.data_len == 4 && memcmp(event.data, "wake", 4) == 0);
  h2_pal_webrtc_event_release(&event);

  // Temporary send pressure is retryable and notified without driving poll.
  EM_ASM(
      { Module.h2WebRtcChannels.get($0).dc.bufferedAmount = 1024 * 1024; },
      (uintptr_t)channel);
  const uint8_t data[] = {1, 2};
  assert(h2_pal_webrtc_channel_send(api, channel, data, sizeof(data), 0) ==
         H2_PAL_ERR_WOULD_BLOCK);
  EM_ASM(
      {
        const dc = Module.h2WebRtcChannels.get($0).dc;
        dc.bufferedAmount = 0;
        dc.onbufferedamountlow();
      },
      (uintptr_t)channel);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_WRITABLE &&
         event.channel == channel);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_channel_send(api, channel, data, sizeof(data), 0) ==
         H2_PAL_OK);
  assert(h2_pal_webrtc_peer_poll(api, peer, 1000, &event) == H2_PAL_OK);
  assert(event.data_len == sizeof(data) &&
         memcmp(event.data, data, sizeof(data)) == 0);
  h2_pal_webrtc_event_release(&event);
  EM_ASM(
      { Module.h2WebRtcPeers.get($0).pc.sctp = {maxMessageSize : 1}; },
      (uintptr_t)peer);
  assert(h2_pal_webrtc_channel_send(api, channel, data, sizeof(data), 0) ==
         H2_PAL_ERR_NO_SPACE);

  // Preserve the accepted FIFO prefix, then report overflow explicitly.
  EM_ASM(
      {
        const dc = Module.h2WebRtcChannels.get($0).dc;
        for (let i = 0; i < 257; ++i)
          dc.onmessage({data : new Uint8Array([ i & 255, i >>> 8 ]).buffer});
      },
      (uintptr_t)channel);
  h2_pal_webrtc_event_t leased = {0};
  for (unsigned i = 0; i < 256; ++i) {
    assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
    assert(event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
    assert(event.data_len == 2 && event.data[0] == i && event.data[1] == 0);
    if (i == 0)
      leased = event;
    else
      h2_pal_webrtc_event_release(&event);
  }
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_ERROR &&
         event.error == H2_PAL_ERR_NO_SPACE);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_ERR_NO_SPACE);
  assert(h2_pal_webrtc_channel_send(api, channel, data, sizeof(data), 0) ==
         H2_PAL_ERR_NO_SPACE);
  h2_pal_webrtc_peer_close(api, peer);
  assert(leased.data_len == 2 && leased.data[0] == 0 && leased.data[1] == 0);
  assert(leased.channel_info.label.len == 1 &&
         leased.channel_info.label.data[0] == 'q');
  h2_pal_webrtc_event_release(&leased);

  channel = create_open_channel();
  EM_ASM(
      {
        const dc = Module.h2WebRtcChannels.get($0).dc;
        const bytes = new Uint8Array(1024 * 1024 - 1);
        for (let i = 0; i < 5; ++i) {
          bytes[0] = i;
          dc.onmessage({data : bytes});
        }
      },
      (uintptr_t)channel);
  for (unsigned i = 0; i < 4; ++i) {
    assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
    assert(event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
    assert(event.data_len == 1024 * 1024 - 1 && event.data[0] == i);
    h2_pal_webrtc_event_release(&event);
  }
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_ERROR &&
         event.error == H2_PAL_ERR_NO_SPACE);
  h2_pal_webrtc_event_release(&event);
  h2_pal_webrtc_peer_close(api, peer);

  channel = create_open_channel();
  EM_ASM(
      {
        const dc = Module.h2WebRtcChannels.get($0).dc;
        globalThis.h2SavedOnMessage = dc.onmessage;
        dc.onerror();
      },
      (uintptr_t)channel);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_STATE &&
         event.channel_state == H2_PAL_WEBRTC_CHANNEL_ERROR);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_channel_send(api, channel, data, sizeof(data), 0) ==
         H2_PAL_ERR_CLOSED);
  h2_pal_webrtc_peer_close(api, peer);
  EM_ASM({ globalThis.h2SavedOnMessage({data : 'late'}); });

  create_peer();
  EM_ASM({
    const dc = new RTCPeerConnection().createDataChannel('allocation-failure');
    const allocate = _malloc;
    try {
      _malloc = () => 0;
      Module.h2WebRtcPeers.get($0).pc.ondatachannel({channel: dc});
    } finally { _malloc = allocate; }
    if (dc.readyState !== 'closed') throw new Error('rejected channel left open');
  }, (uintptr_t)peer);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_ERROR && event.error == H2_PAL_ERR_NO_MEMORY);
  h2_pal_webrtc_event_release(&event);
  h2_pal_webrtc_peer_close(api, peer);

  create_peer();
  observed = 0;
  EM_ASM({ setTimeout(() => Module._web_test_close_while_waiting(), 2); });
  start = emscripten_get_now();
  assert(h2_pal_webrtc_peer_poll(api, peer, 1000, &event) == H2_PAL_ERR_CLOSED);
  assert(peer == NULL && observed == 1 && emscripten_get_now() - start < 750);
}

int main(void) {
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  platform = h2_web_platform_create(&config);
  assert(platform != NULL);
  api = h2_web_platform_webrtc_api(platform);
  EM_ASM({ globalThis.h2FakeCreateMedia(7); });
  h2_pal_webrtc_track_t *track = calloc(1, sizeof(*track));
  assert(track != NULL);
  track->native_handle = (void *)(uintptr_t)7;
  h2_pal_webrtc_track_t stale = *track;
  h2_pal_webrtc_track_t invalid = {.native_handle = (void *)(uintptr_t)99};
  create_peer();
  assert(h2_pal_webrtc_peer_set_track(api, peer, &invalid) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_webrtc_peer_set_track(api, peer, track) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_set_track(api, peer, track) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_webrtc_peer_unset_track(api, peer, &stale) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_pal_webrtc_peer_t *second = NULL;
  assert(h2_pal_webrtc_peer_create(api, &second) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_set_track(api, second, &stale) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_pal_webrtc_peer_close(api, second);

  int offer_rc = h2_pal_webrtc_peer_start_offer(api, peer);
  assert(offer_rc == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_INVALID_STATE);
  const h2_pal_webrtc_ice_server_t ice = {
      .url = {.data = "stun:late", .len = 9}};
  assert(h2_pal_webrtc_peer_add_ice_server(api, peer, &ice) ==
         H2_PAL_ERR_INVALID_STATE);
  const h2_pal_webrtc_str_t answer = {.data = "fake-answer", .len = 11};
  assert(h2_pal_webrtc_peer_set_remote_sdp(api, peer, H2_PAL_WEBRTC_SDP_ANSWER,
                                           answer) == H2_PAL_OK);
  // Browser media delivery is independent of consuming C events.
  assert(EM_ASM_INT({ return globalThis.h2FakeAudioPlayCount || 0; }) == 1);
  assert(EM_ASM_INT({ return globalThis.h2FakeGetUserMediaCount || 0; }) == 0);
  drain();

  // A rejected detach does not release the borrowed Track or its ownership.
  EM_ASM({ globalThis.h2FakeDetachReject = true; });
  assert(h2_pal_webrtc_peer_unset_track(api, peer, track) == H2_PAL_ERR_IO);
  assert(EM_ASM_INT({ return Module.h2WebRtcTrackOwners.has(7); }));
  observed = 0;
  EM_ASM(
      {
        const pc = Module.h2WebRtcPeers.get($0).pc;
        globalThis.h2SavedOnTrack = pc.ontrack;
        setTimeout(() =>
                        {
                          pc.ontrack({track : {kind : 'audio'}, streams : []});
                          Module._web_test_destroy_while_waiting();
                        },
                   1);
      },
      (uintptr_t)peer);
  assert(h2_pal_webrtc_peer_unset_track(api, peer, track) == H2_PAL_OK);
  assert(observed == 1);
  assert(EM_ASM_INT({ return globalThis.h2FakeDetachResolved; }));
  free(track);
  track = NULL;
  assert(EM_ASM_INT({
    const media = Module.h2WebRtcTracks.get(7);
    return !Module.h2WebRtcTrackOwners.has(7) && media.audio.paused &&
               media.audio.srcObject === null &&
                                          !media.stream.getTracks()[0].stopped;
  }));
  EM_ASM(
      { globalThis.h2SavedOnTrack({track : {kind : 'audio'}, streams : []}); });
  assert(EM_ASM_INT({ return globalThis.h2FakeAudioPlayCount; }) == 1);
  // The peer remains usable after unbinding; a new Track cannot be negotiated
  // through set_track after the initial offer.
  assert(h2_pal_webrtc_peer_set_track(api, peer, &stale) ==
         H2_PAL_ERR_INVALID_STATE);
  const h2_pal_webrtc_channel_config_t dc = {
      .label = {.data = "still-live", .len = 10}, .ordered = 1, .reliable = 1};
  h2_pal_webrtc_channel_t *channel = NULL;
  assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &dc, &channel) ==
         H2_PAL_OK);
  h2_pal_webrtc_channel_close(api, channel);
  h2_pal_webrtc_peer_close(api, peer);
  EM_ASM(
      { globalThis.h2SavedOnTrack({track : {kind : 'audio'}, streams : []}); });
  assert(EM_ASM_INT({ return globalThis.h2FakeAudioPlayCount; }) == 1);

  // Ownership can be reused by a different caller-owned C Track/Peer.
  create_peer();
  assert(h2_pal_webrtc_peer_set_track(api, peer, &stale) == H2_PAL_OK);
  observed = 0;
  EM_ASM({ setTimeout(() => Module._web_test_close_while_waiting(), 1); });
  assert(h2_pal_webrtc_peer_unset_track(api, peer, &stale) ==
         H2_PAL_ERR_CLOSED);
  assert(peer == NULL && observed == 1);
  assert(EM_ASM_INT({ return !Module.h2WebRtcTrackOwners.has(7); }));

  // Close must release an outstanding ICE gather wait, not defer pc.close
  // until a promise that can no longer complete has returned.
  EM_ASM({ globalThis.h2FakeWaitIce = true; });
  create_peer();
  observed = 0;
  EM_ASM({ setTimeout(() => Module._web_test_close_while_waiting(), 1); });
  assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_CLOSED);
  assert(peer == NULL && observed == 1);
  EM_ASM({
    globalThis.h2FakeWaitIce = false;
    globalThis.h2FakeRemoteDelay = true;
  });
  create_peer();
  observed = 0;
  EM_ASM({ setTimeout(() => Module._web_test_close_while_waiting(), 1); });
  assert(h2_pal_webrtc_peer_set_remote_sdp(api, peer, H2_PAL_WEBRTC_SDP_ANSWER,
                                           answer) == H2_PAL_ERR_CLOSED);
  assert(peer == NULL && observed == 1);
  EM_ASM({ globalThis.h2FakeRemoteDelay = false; });

  // A playback-only application supplies its own audio element too.
  EM_ASM({ Module.h2WebRtcTracks.set(8, {audio : new Audio()}); });
  h2_pal_webrtc_track_t playback = {.native_handle = (void *)(uintptr_t)8};
  create_peer();
  assert(h2_pal_webrtc_peer_set_track(api, peer, &playback) == H2_PAL_OK);
  assert(EM_ASM_INT({ return globalThis.h2FakeRecvOnly; }));
  assert(h2_pal_webrtc_peer_unset_track(api, peer, &playback) == H2_PAL_OK);
  h2_pal_webrtc_peer_close(api, peer);

  // One failed sender must not return while another detach is still running.
  EM_ASM({
    const media = globalThis.h2FakeCreateMedia(9);
    media.stream.tracks.push({kind: 'audio', stop() { throw new Error('borrowed'); }
});
globalThis.h2FakeDetachReject = true;
});
h2_pal_webrtc_track_t multiple = {.native_handle = (void *)(uintptr_t)9};
create_peer();
assert(h2_pal_webrtc_peer_set_track(api, peer, &multiple) == H2_PAL_OK);
assert(h2_pal_webrtc_peer_unset_track(api, peer, &multiple) == H2_PAL_ERR_IO);
assert(EM_ASM_INT({
  return globalThis.h2FakeDetachResolved && Module.h2WebRtcTrackOwners.has(9);
}));
assert(h2_pal_webrtc_peer_unset_track(api, peer, &multiple) == H2_PAL_OK);
h2_pal_webrtc_peer_close(api, peer);
test_events();
h2_web_platform_destroy(platform);
assert(EM_ASM_INT({
  return Module.h2WebRtcPeers.size === 0 &&
                                        Module.h2WebRtcTrackOwners.size === 0;
}));
puts("Web Track, event FIFO/budgets, wake and backpressure tests passed (fake "
     "browser)");
return 0;
}
