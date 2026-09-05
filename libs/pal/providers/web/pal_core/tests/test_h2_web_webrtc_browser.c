#include "h2_web_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Real browser primitives only: no RTCPeerConnection or media API substitutes.
// clang-format off
EM_ASYNC_JS(void, prepare_media, (), {
  const context = new AudioContext();
  await context.resume();
  const oscillator = context.createOscillator();
  oscillator.frequency.value = 440;
  const source = context.createMediaStreamDestination();
  oscillator.connect(source);
  oscillator.start();
  const audio = document.createElement('audio');
  audio.muted = true;
  Module.h2WebRtcTracks = new Map([[7, {stream: source.stream, audio}]]);
  let remote = null;
  let remoteAudio = null;
  if (!Module.pion) {
    // Constrain the native remote only; leave the PAL-owned connection's
    // configuration and SDP entirely under the production provider.
    remote = new RTCPeerConnection(Module.relay ? {
      iceTransportPolicy: 'relay',
      iceServers: [{urls: Module.iceURL, username: 'h2peer', credential: 'h2peer-secret'}]
    } : {iceServers: [{urls: Module.stunURL}]});
    remoteAudio = document.createElement('audio');
    remoteAudio.muted = true;
    remote.ontrack = ({track, streams}) => {
      remoteAudio.srcObject = streams[0] || new MediaStream([track]);
      remoteAudio.play();
    };
    remote.addTrack(source.stream.getAudioTracks()[0], source.stream);
    remote.ondatachannel = ({channel}) => {
      Module.remoteChannel = channel;
      channel.binaryType = 'arraybuffer';
      channel.onmessage = event => channel.send(event.data);
    };
  }
  Module.browserFixture = {context, oscillator, source, audio, remote, remoteAudio};
});

EM_ASYNC_JS(char *, answer_offer, (const char *sdp, size_t len), {
  const remote = Module.browserFixture.remote;
  let text;
  if (Module.pion) {
    const response = await fetch('/pion/offer', {
      method: 'POST', body: UTF8ToString(sdp, len), signal: AbortSignal.timeout(15000)
    });
    if (!response.ok) throw new Error('Pion offer failed: ' + response.status);
    text = await response.text();
  } else {
    await remote.setRemoteDescription({type: 'offer', sdp: UTF8ToString(sdp, len)});
    await remote.setLocalDescription(await remote.createAnswer());
    if (remote.iceGatheringState !== 'complete') {
      await new Promise(resolve => {
        const changed = () => {
          if (remote.iceGatheringState === 'complete') {
            remote.removeEventListener('icegatheringstatechange', changed);
            resolve();
          }
        };
        remote.addEventListener('icegatheringstatechange', changed);
        changed();
      });
    }
    text = remote.localDescription.sdp;
  }
  const bytes = lengthBytesUTF8(text) + 1;
  const buffer = _malloc(bytes);
  if (!buffer) throw new Error('answer allocation failed');
  stringToUTF8(text, buffer, bytes);
  return buffer;
});

EM_ASYNC_JS(int, verify_audio, (uintptr_t address), {
  const local = Module.h2WebRtcPeers.get(address).pc;
  const {remote, audio, context} = Module.browserFixture;
  const peers = remote ? [local, remote] : [local];
  // Measure decoded receiver PCM before the deliberately muted playback sinks.
  const probes = peers.map(pc => {
    const receiver = pc.getReceivers().find(r => r.track.kind === 'audio');
    const source = context.createMediaStreamSource(new MediaStream([receiver.track]));
    const analyser = context.createAnalyser();
    const silence = context.createGain();
    silence.gain.value = 0;
    source.connect(analyser);
    analyser.connect(silence);
    silence.connect(context.destination);
    return {source, analyser, silence, samples: new Float32Array(analyser.fftSize)};
  });
  const release = () => probes.forEach(p => {
    p.source.disconnect(); p.analyser.disconnect(); p.silence.disconnect();
  });
  const deadline = performance.now() + 10000;
  while (performance.now() < deadline) {
    const received = async pc => {
      const stats = await pc.getStats();
      return [...stats.values()].some(s => s.type === 'inbound-rtp' &&
          s.kind === 'audio' && s.packetsReceived > 10);
    };
    const pcm = probes.map(p => {
      p.analyser.getFloatTimeDomainData(p.samples);
      return p.samples.some(sample => Math.abs(sample) > 0.001);
    });
    if ((await Promise.all(peers.map(received))).every(Boolean) &&
        pcm.every(Boolean) && audio.srcObject && !audio.paused && audio.currentTime > 0) {
      release();
      return 1;
    }
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  for (const pc of peers) {
    const stats = await pc.getStats();
    for (const stat of stats.values())
      if (stat.type === 'inbound-rtp' || stat.type === 'outbound-rtp' || stat.type === 'media-source') out(JSON.stringify(stat));
  }
  out(JSON.stringify({paused: audio.paused, time: audio.currentTime, source: !!audio.srcObject,
      context: Module.browserFixture.context.state}));
  release();
  return 0;
});

EM_ASYNC_JS(int, verify_relay, (uintptr_t address), {
  if (!Module.relay) return 1;
  const local = Module.h2WebRtcPeers.get(address).pc;
  const remote = Module.browserFixture.remote;
  const selected = async pc => {
    const stats = await pc.getStats();
    const transport = [...stats.values()].find(s => s.type === 'transport' && s.selectedCandidatePairId);
    const pair = transport && stats.get(transport.selectedCandidatePairId);
    if (!pair) return null;
    const a = stats.get(pair.localCandidateId);
    const b = stats.get(pair.remoteCandidateId);
    return {local: a.candidateType, remote: b.candidateType,
            sent: pair.bytesSent, received: pair.bytesReceived, state: pair.state};
  };
  const a = await selected(local);
  const b = await selected(remote);
  out('WEB_BROWSER relay-pairs=' + JSON.stringify({pal: a, native: b}));
  return a && b && a.remote === 'relay' && b.local === 'relay' &&
      [a, b].every(p => p.state === 'succeeded' && p.sent > 0 && p.received > 0);
});

EM_ASYNC_JS(void, cleanup_browser, (), {
  const {remote, source, oscillator, context, remoteAudio} = Module.browserFixture;
  if (remote) remote.close();
  if (remoteAudio) {
    remoteAudio.pause();
    remoteAudio.srcObject = null;
  }
  oscillator.stop();
  source.stream.getTracks().forEach(track => track.stop());
  await context.close();
  Module.h2WebRtcTracks.clear();
});

EM_ASYNC_JS(void, connection_diagnostics, (uintptr_t address), {
  const local = Module.h2WebRtcPeers.get(address).pc;
  const remote = Module.browserFixture.remote;
  for (const pc of remote ? [local, remote] : [local]) {
    out(JSON.stringify({connection: pc.connectionState, ice: pc.iceConnectionState,
        gathering: pc.iceGatheringState, signaling: pc.signalingState}));
    const stats = await pc.getStats();
    for (const stat of stats.values()) {
      if (['local-candidate', 'remote-candidate', 'candidate-pair'].includes(stat.type))
        out(JSON.stringify(stat));
    }
  }
});

EM_ASYNC_JS(void, close_remote, (), {
  if (Module.pion) {
    const response = await fetch('/pion/close', {
      method: 'POST', signal: AbortSignal.timeout(10000)
    });
    if (!response.ok) throw new Error('Pion close failed: ' + response.status);
  } else {
    Module.remoteChannel.close();
  }
});

EM_JS(int, detached, (uintptr_t address), {
  const entry = Module.h2WebRtcPeers.get(address);
  const {source, audio} = Module.browserFixture;
  return !entry.binding && !Module.h2WebRtcTrackOwners.has(7) &&
      entry.pc.getSenders().every(sender => sender.track === null) &&
      source.stream.getAudioTracks().every(track => track.readyState === 'live') &&
      audio.srcObject === null && audio.paused;
});

EM_JS(char *, copy_ice_url, (), {
  const size = lengthBytesUTF8(Module.iceURL) + 1;
  const url = _malloc(size);
  if (url) stringToUTF8(Module.iceURL, url, size);
  return url;
});

EM_JS(int, gathered_relay, (const char *sdp, size_t len), {
  return UTF8ToString(sdp, len).split(String.fromCharCode(10)).some(line =>
      line.startsWith('a=candidate:') && / typ relay(?: |$)/.test(line.trim()));
});
// clang-format on

static h2_web_platform_t *platform;
static const h2_pal_webrtc_api_t *api;
static h2_pal_webrtc_peer_t *peer;
static h2_pal_webrtc_channel_t *channel;
static int pion;

static void assert_echo(const h2_pal_webrtc_event_t *event, const uint8_t *data,
                        size_t len, int text) {
  const char *prefix =
      pion ? (text ? "server-echo-text:" : "server-echo-binary:") : "";
  const size_t prefix_len = strlen(prefix);
  assert(event->channel == channel && event->is_text == text);
  assert(event->data_len == prefix_len + len);
  assert(memcmp(event->data, prefix, prefix_len) == 0);
  assert(memcmp(event->data + prefix_len, data, len) == 0);
}

static h2_pal_webrtc_event_t next_kind(h2_pal_webrtc_event_kind_t kind) {
  printf("WEB_BROWSER wait-kind=%d\n", (int)kind);
  const double deadline = emscripten_get_now() + 10000;
  while (emscripten_get_now() < deadline) {
    h2_pal_webrtc_event_t event = {0};
    const h2_pal_result_t rc = h2_pal_webrtc_peer_poll(api, peer, 100, &event);
    if (rc == H2_PAL_ERR_TIMEOUT)
      continue;
    assert(rc == H2_PAL_OK);
    printf("WEB_BROWSER event-kind=%d peer-state=%d channel-state=%d\n",
           (int)event.kind, (int)event.peer_state, (int)event.channel_state);
    assert(event.kind != H2_PAL_WEBRTC_EVENT_ERROR);
    assert(event.kind != H2_PAL_WEBRTC_EVENT_PEER_STATE ||
           event.peer_state != H2_PAL_WEBRTC_PEER_FAILED);
    if (event.kind == kind)
      return event;
    h2_pal_webrtc_event_release(&event);
  }
  connection_diagnostics((uintptr_t)peer);
  assert(!"browser event deadline");
  return (h2_pal_webrtc_event_t){0};
}

static h2_pal_webrtc_event_t echo(const uint8_t *data, size_t len, int text) {
  assert(h2_pal_webrtc_channel_send(api, channel, data, len, text) ==
         H2_PAL_OK);
  // Let browser transport and provider callbacks run without application poll.
  emscripten_sleep(100);
  h2_pal_webrtc_event_t event = next_kind(H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
  assert_echo(&event, data, len, text);
  return event;
}

static void test_backpressure(void) {
  uint8_t data[16384];
  unsigned sent = 0;
  h2_pal_result_t rc = H2_PAL_OK;
  for (; sent < 128; ++sent) {
    memset(data, (int)sent, sizeof(data));
    rc = h2_pal_webrtc_channel_send(api, channel, data, sizeof(data), 0);
    if (rc != H2_PAL_OK)
      break;
  }
  assert(rc == H2_PAL_ERR_WOULD_BLOCK && sent > 0);
  unsigned received = 0;
  int writable = 0;
  const double deadline = emscripten_get_now() + 10000;
  while ((received < sent || !writable) && emscripten_get_now() < deadline) {
    h2_pal_webrtc_event_t event = {0};
    rc = h2_pal_webrtc_peer_poll(api, peer, 100, &event);
    if (rc == H2_PAL_ERR_TIMEOUT)
      continue;
    assert(rc == H2_PAL_OK && event.kind != H2_PAL_WEBRTC_EVENT_ERROR);
    if (event.kind == H2_PAL_WEBRTC_EVENT_WRITABLE) {
      assert(event.channel == channel);
      writable = 1;
    } else if (event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE) {
      assert(received < sent && event.channel == channel && !event.is_text);
      memset(data, (int)received, sizeof(data));
      assert_echo(&event, data, sizeof(data), 0);
      received++;
    }
    h2_pal_webrtc_event_release(&event);
  }
  assert(received == sent && writable);
  memset(data, 255, sizeof(data));
  h2_pal_webrtc_event_t event = echo(data, sizeof(data), 0);
  h2_pal_webrtc_event_release(&event);
  printf("WEB_BROWSER backpressure=WOULD_BLOCK writable=received fifo=%u "
         "retry=PASS\n",
         sent);
}

int main(void) {
  pion = EM_ASM_INT({ return Module.pion ? 1 : 0; });
  printf("WEB_BROWSER backend=%s\n", pion ? "pion" : "browser");
  const int relay = EM_ASM_INT({ return Module.relay ? 1 : 0; });
  printf("WEB_BROWSER relay=%d\n", relay);
  prepare_media();
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  platform = h2_web_platform_create(&config);
  assert(platform != NULL);
  api = h2_web_platform_webrtc_api(platform);
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  char *ice_url = copy_ice_url();
  assert(ice_url != NULL);
  const h2_pal_webrtc_ice_server_t ice = {
      .url = {.data = ice_url, .len = strlen(ice_url)},
      // Public credentials of the repository's isolated local TURN fixture.
      .username = {.data = relay ? "h2peer" : NULL, .len = relay ? 6 : 0},
      .credential = {.data = relay ? "h2peer-secret" : NULL,
                     .len = relay ? 13 : 0}};
  assert(h2_pal_webrtc_peer_add_ice_server(api, peer, &ice) == H2_PAL_OK);
  free(ice_url);
  h2_pal_webrtc_track_t *track = calloc(1, sizeof(*track));
  assert(track != NULL);
  track->native_handle = (void *)(uintptr_t)7;
  assert(h2_pal_webrtc_peer_set_track(api, peer, track) == H2_PAL_OK);
  const h2_pal_webrtc_channel_config_t dc = {
      .label = {.data = "browser", .len = 7}, .ordered = 1, .reliable = 1};
  assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &dc, &channel) ==
         H2_PAL_OK);
  assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
  h2_pal_webrtc_event_t event = next_kind(H2_PAL_WEBRTC_EVENT_LOCAL_SDP);
  if (relay)
    assert(gathered_relay(event.sdp.data, event.sdp.len));
  char *answer = answer_offer(event.sdp.data, event.sdp.len);
  h2_pal_webrtc_event_release(&event);
  assert(answer != NULL);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             api, peer, H2_PAL_WEBRTC_SDP_ANSWER,
             (h2_pal_webrtc_str_t){.data = answer, .len = strlen(answer)}) ==
         H2_PAL_OK);
  free(answer);
  event = next_kind(H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
  assert(event.channel == channel &&
         event.channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN);
  h2_pal_webrtc_event_release(&event);
  const uint8_t text[] = "real browser echo";
  event = echo(text, sizeof(text) - 1, 1);
  h2_pal_webrtc_event_release(&event);
  assert(verify_audio((uintptr_t)peer));
  puts(pion ? "WEB_BROWSER audio=pion-opus-roundtrip-nonsilent-pcm "
              "datachannel=text PASS"
            : "WEB_BROWSER audio=bidirectional-nonsilent-decoded-pcm "
              "datachannel=text PASS");
  test_backpressure();

  assert(h2_pal_webrtc_peer_unset_track(api, peer, track) == H2_PAL_OK);
  free(track);
  // Successful unset has awaited native replaceTrack promises; user source
  // stays live.
  assert(detached((uintptr_t)peer));
  const uint8_t binary[] = {0, 1, 255, 128, 0, 42};
  h2_pal_webrtc_event_t leased = echo(binary, sizeof(binary), 0);
  assert(detached((uintptr_t)peer));
  assert(verify_relay((uintptr_t)peer));
  close_remote();
  event = next_kind(H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
  // The fixture closes the entire Pion association (SCTP ABORT), whereas the
  // browser peer closes only this DataChannel with a graceful stream reset.
  assert(event.channel == channel &&
         event.channel_state == (pion ? H2_PAL_WEBRTC_CHANNEL_ERROR
                                      : H2_PAL_WEBRTC_CHANNEL_CLOSED));
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_channel_send(api, channel, binary, sizeof(binary), 0) ==
         H2_PAL_ERR_CLOSED);
  emscripten_sleep(100);
  h2_pal_result_t rc;
  while ((rc = h2_pal_webrtc_peer_poll(api, peer, 0, &event)) == H2_PAL_OK) {
    assert(event.kind != H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
    h2_pal_webrtc_event_release(&event);
  }
  assert(rc == H2_PAL_ERR_WOULD_BLOCK);
  printf("WEB_BROWSER remote-terminal=%s send=CLOSED duplicate-terminal=none\n",
         pion ? "ERROR" : "CLOSED");
  h2_pal_webrtc_peer_close(api, peer);
  h2_web_platform_destroy(platform);
  assert_echo(&leased, binary, sizeof(binary), 0);
  assert(leased.channel_info.label.len == 7 &&
         memcmp(leased.channel_info.label.data, "browser", 7) == 0);
  h2_pal_webrtc_event_release(&leased);
  cleanup_browser();
  puts("WEB_BROWSER unset=detached user-track=live post-unset-echo=binary "
       "remote-close=received event-lease=released PASS");
  puts("WEB_BROWSER_COMPLETE");
  return 0;
}
