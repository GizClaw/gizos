#include "h2_web_main_thread.h"
#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Embedded JS operators are not understood by the C formatter.
#define H2_WEB_WEBRTC_EVENT_LIMIT 256u
#define H2_WEB_WEBRTC_BYTE_LIMIT (4u * 1024u * 1024u)

struct h2_pal_webrtc_channel {
  h2_pal_webrtc_peer_t *peer;
  h2_pal_webrtc_channel_t *next;
  h2_pal_webrtc_channel_info_t info;
  char *label;
  bool terminal;
};

typedef struct h2_web_webrtc_event {
  struct h2_web_webrtc_event *next;
  h2_pal_webrtc_event_t event;
  char *label;
  uint8_t *payload;
  size_t bytes;
  h2_pal_mem_api_t allocator;
} h2_web_webrtc_event_t;

static char *h2_web_webrtc_copy_string(const h2_pal_mem_api_t *memory,
                                     const char *data, size_t len);

struct h2_pal_webrtc_peer {
  h2_web_platform_t *owner;
  h2_pal_mem_api_t allocator;
  h2_pal_webrtc_peer_t *next;
  h2_web_webrtc_event_t *event_head;
  h2_web_webrtc_event_t *event_tail;
  h2_pal_webrtc_channel_t *channels;
  size_t event_count;
  size_t event_bytes;
  h2_pal_result_t event_error;
  bool error_reported;
  bool poll_waiting;
  h2_web_async_t *poll_op;
  // Offer/answer/unset waits: pc.close() leaves their Promises unsettled, so
  // peer_close completes them with CLOSED.
  struct h2_web_webrtc_control *controls;
  h2_pal_webrtc_peer_state_t state;
  h2_pal_webrtc_track_t *media_track;
  // Opus tracks: a media task moves packets between the track and the
  // browser's encoded transforms.
  h2_pal_task_t *media_task;
  h2_web_async_t *media_op;
  bool opus_mode;
  bool opus_send_blocked;
  bool media_stop;
  bool media_in_callback;
  bool offer_started;
  bool track_detaching;
  unsigned async_calls;
  bool closed;
  bool close_pending;
};

// A browser event wakes a task waiting in peer_poll through its completion
// condition.
static void h2_web_webrtc_wake(h2_pal_webrtc_peer_t *peer) {
  H2_WEB_STATE_GUARD();
  if (peer->poll_op != NULL)
    h2_web_async_signal(peer->owner, peer->poll_op, H2_PAL_OK);
}

EMSCRIPTEN_KEEPALIVE void h2_web_webrtc_fail(uintptr_t address, int error) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)address;
  if (peer == NULL || peer->closed || peer->event_error != H2_PAL_OK)
    return;
  peer->event_error = (h2_pal_result_t)error;
  h2_web_webrtc_wake(peer);
}

static void h2_web_webrtc_event_release(h2_pal_webrtc_event_t *event) {
  H2_WEB_STATE_GUARD();
  if (event == NULL || event->_private == NULL)
    return;
  h2_web_webrtc_event_t *node = event->_private;
  h2_pal_mem_api_t memory = node->allocator;
  h2_pal_mem_free(&memory, node->label);
  h2_pal_mem_free(&memory, node->payload);
  h2_pal_mem_free(&memory, node);
  memset(event, 0, sizeof(*event));
}

static int h2_web_webrtc_enqueue(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_event_kind_t kind,
    h2_pal_webrtc_channel_t *channel, h2_pal_webrtc_channel_state_t state,
    h2_pal_webrtc_peer_state_t peer_state, h2_pal_webrtc_sdp_type_t sdp_type,
    const void *payload, size_t payload_len, int is_text) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || (payload == NULL && payload_len != 0u) ||
      payload_len == SIZE_MAX)
    return 0;
  if (peer->event_error != H2_PAL_OK)
    return 0;
  const size_t label_len = channel == NULL ? 0u : channel->info.label.len;
  if (peer->event_count >= H2_WEB_WEBRTC_EVENT_LIMIT ||
      payload_len > H2_WEB_WEBRTC_BYTE_LIMIT - peer->event_bytes ||
      label_len > H2_WEB_WEBRTC_BYTE_LIMIT - peer->event_bytes - payload_len) {
    h2_web_webrtc_fail((uintptr_t)peer, H2_PAL_ERR_NO_SPACE);
    return 0;
  }
  h2_web_webrtc_event_t *node = h2_pal_mem_alloc(&peer->allocator, sizeof(*node));
  if (node == NULL) {
    h2_web_webrtc_fail((uintptr_t)peer, H2_PAL_ERR_NO_MEMORY);
    return 0;
  }
  memset(node, 0, sizeof(*node));
  node->allocator = peer->allocator;
  node->bytes = payload_len + label_len;
  node->event.kind = kind;
  node->event.peer = peer;
  node->event.channel = channel;
  node->event.channel_state = state;
  node->event.peer_state = peer_state;
  node->event.sdp_type = sdp_type;
  node->event.is_text = is_text;
  if (channel != NULL) {
    node->label = h2_web_webrtc_copy_string(&peer->allocator, channel->info.label.data,
                                            channel->info.label.len);
    if (node->label == NULL)
      goto fail;
    node->event.channel_info = channel->info;
    node->event.channel_info.label.data = node->label;
  }
  if (payload_len != 0u) {
    node->payload = h2_pal_mem_alloc(&peer->allocator, payload_len + 1u);
    if (node->payload == NULL)
      goto fail;
    memcpy(node->payload, payload, payload_len);
    node->payload[payload_len] = 0u;
    node->event.data = node->payload;
    node->event.data_len = payload_len;
  }
  if (kind == H2_PAL_WEBRTC_EVENT_LOCAL_SDP) {
    node->event.sdp.data = (const char *)node->payload;
    node->event.sdp.len = payload_len;
    node->event.data = NULL;
    node->event.data_len = 0u;
  }
  if (peer->event_tail == NULL)
    peer->event_head = node;
  else
    peer->event_tail->next = node;
  peer->event_tail = node;
  peer->event_count++;
  peer->event_bytes += node->bytes;
  h2_web_webrtc_wake(peer);
  return 1;
fail: {
    h2_pal_mem_api_t memory = node->allocator;
    h2_pal_mem_free(&memory, node->label);
    h2_pal_mem_free(&memory, node->payload);
    h2_pal_mem_free(&memory, node);
  }
  h2_web_webrtc_fail((uintptr_t)peer, H2_PAL_ERR_NO_MEMORY);
  return 0;
}

static h2_pal_result_t h2_web_webrtc_dequeue(h2_pal_webrtc_peer_t *peer,
                                             h2_pal_webrtc_event_t *out_event) {
  H2_WEB_STATE_GUARD();
  h2_web_webrtc_event_t *node = peer->event_head;
  if (node == NULL) {
    if (peer->event_error != H2_PAL_OK) {
      if (peer->error_reported)
        return peer->event_error;
      peer->error_reported = true;
      *out_event = (h2_pal_webrtc_event_t){
          .kind = H2_PAL_WEBRTC_EVENT_ERROR,
          .peer = peer,
          .error = peer->event_error,
      };
      return H2_PAL_OK;
    }
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  peer->event_head = node->next;
  peer->event_count--;
  peer->event_bytes -= node->bytes;
  if (peer->event_head == NULL)
    peer->event_tail = NULL;
  *out_event = node->event;
  out_event->_private = node;
  out_event->_release = h2_web_webrtc_event_release;
  return H2_PAL_OK;
}

static char *h2_web_webrtc_copy_string(const h2_pal_mem_api_t *memory,
                                     const char *data, size_t len) {
  if ((data == NULL && len != 0u) || len == SIZE_MAX)
    return NULL;
  char *copy = h2_pal_mem_alloc(memory, len + 1u);
  if (copy == NULL)
    return NULL;
  if (len != 0u)
    memcpy(copy, data, len);
  copy[len] = '\0';
  return copy;
}

static h2_pal_webrtc_channel_t *
h2_web_webrtc_find_channel(h2_pal_webrtc_peer_t *peer, uintptr_t address) {
  H2_WEB_STATE_GUARD();
  for (h2_pal_webrtc_channel_t *channel = peer == NULL ? NULL : peer->channels;
       channel != NULL; channel = channel->next) {
    if ((uintptr_t)channel == address)
      return channel;
  }
  return NULL;
}

/* clang-format off */
EM_JS(void, h2_web_webrtc_peer_create_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], "i32",
    (peer_address) => {
  if (typeof globalThis.RTCPeerConnection !== 'function')
    return -3;
  const peers = Module['h2WebRtcPeers'] ||= new Map();
  const channels = Module['h2WebRtcChannels'] ||= new Map();
  try {
    const pc = new RTCPeerConnection();
    const entry = {
      pc,
      iceServers : [],
      binding : null,
      cancelOffer : null,
      dtlsErrors : new Map()
    };
    peers.set(peer_address, entry);
    entry.watchDtlsErrors = () => {
      const transports = [pc.sctp?.transport,
        ...(pc.getSenders?.() || []).map(sender => sender.transport),
        ...(pc.getReceivers?.() || []).map(receiver => receiver.transport)];
      for (const transport of transports) {
        if (!transport?.addEventListener || entry.dtlsErrors.has(transport))
          continue;
        const listener = event => {
          if (peers.get(peer_address) !== entry) return;
          const error = event.error;
          const verifiedFailure = error?.errorDetail === 'fingerprint-failure' ||
            (error?.errorDetail === 'dtls-failure' && (error.sentAlert === 42 || error.sentAlert === 46));
          Module['_h2_web_webrtc_fail'](peer_address, verifiedFailure ? -17 : -4);
        };
        entry.dtlsErrors.set(transport, listener);
        transport.addEventListener('error', listener);
      }
    };
    const bindChannel = (channelAddress, dc) => {
      const channelEntry = {peerAddress : peer_address, dc};
      channels.set(channelAddress, channelEntry);
      dc.binaryType = 'arraybuffer';
      dc.bufferedAmountLowThreshold = 0;
      const live = () => channels.get(channelAddress) === channelEntry &&
          peers.get(peer_address) === entry;
      dc.onopen = () => {
        if (!live())
          return;
        Module['_h2_web_webrtc_channel_metadata'](peer_address, channelAddress,
                                                  dc.id == null ? 0 : dc.id,
                                                  dc.id == null ? 0 : 1);
        Module['_h2_web_webrtc_channel_state'](peer_address, channelAddress, 1);
      };
      dc.onbufferedamountlow = () => {
        if (live() &&dc.readyState === 'open')
          Module['_h2_web_webrtc_channel_writable'](peer_address,
                                                    channelAddress);
      };
      const terminal = (state) => {
        if (!live())
          return;
        channels.delete(channelAddress);
        dc.onbufferedamountlow = null;
        Module['_h2_web_webrtc_channel_state'](peer_address, channelAddress,
                                               state);
        if (state === 3) {
          dc.onopen = dc.onclose = dc.onerror = dc.onmessage = null;
          try { dc.close(); }
          catch(_) {}
        }
      };
      dc.onclose = () => terminal(2);
      dc.onerror = () => terminal(3);
      dc.onmessage = (event) => {
        if (!live())
          return;
        let bytes;
        let isText = 0;
        if (typeof event.data === 'string') {
          bytes = new TextEncoder().encode(event.data);
          isText = 1;
        } else if (event.data instanceof ArrayBuffer) {
          bytes = new Uint8Array(event.data);
        } else if (ArrayBuffer.isView(event.data)) {
          bytes = new Uint8Array(event.data.buffer, event.data.byteOffset,
                                 event.data.byteLength);
        } else {
          terminal(3);
          return;
        }
        if (bytes.byteLength > 4 * 1024 * 1024) {
          Module['_h2_web_webrtc_fail'](peer_address, -13);
          terminal(3);
          return;
        }
        const buffer = bytes.byteLength ? _malloc(bytes.byteLength) : 0;
        if (bytes.byteLength && !buffer) {
          Module['_h2_web_webrtc_fail'](peer_address, -5);
          terminal(3);
          return;
        }
        if (bytes.byteLength)
          HEAPU8.set(bytes, buffer);
        Module['_h2_web_webrtc_channel_message'](
            peer_address, channelAddress, buffer, bytes.byteLength, isText);
        if (buffer)
          _free(buffer);
      };
    };
    entry.bindChannel = bindChannel;
    pc.onconnectionstatechange = () => {
      if (peers.get(peer_address) !== entry)
        return;
      const states = {
        new : 0,
        connecting : 1,
        connected : 2,
        disconnected : 3,
        failed : 4,
        closed : 5
      };
      Module['_h2_web_webrtc_peer_state'](
          peer_address, states[pc.connectionState] ?? 4);
    };
    pc.ondatachannel = (event) => {
      if (peers.get(peer_address) !== entry)
        return;
      const dc = event.channel;
      const labelBytes = new TextEncoder().encode(dc.label);
      const label = labelBytes.byteLength ? _malloc(labelBytes.byteLength) : 0;
      if (labelBytes.byteLength && !label) {
        Module['_h2_web_webrtc_fail'](peer_address, -5);
        try { dc.close(); }
        catch(_) {}
        return;
      }
      if (labelBytes.byteLength)
        HEAPU8.set(labelBytes, label);
      const channelAddress = Module['_h2_web_webrtc_remote_channel'](
          peer_address, label, labelBytes.byteLength, dc.id == null ? 0 : dc.id,
          dc.id == null ? 0 : 1, dc.ordered ? 1 : 0,
          dc.maxRetransmits == null && dc.maxPacketLifeTime == null ? 1 : 0);
      if (label)
        _free(label);
      if (!channelAddress) {
        try { dc.close(); }
        catch(_) {}
        return;
      }
      bindChannel(channelAddress, dc);
      if (dc.readyState === 'open')
        Promise.resolve().then(() => dc.onopen?.());
    };
    pc.ontrack = (event) => {
      const binding = entry.binding;
      if (peers.get(peer_address) !== entry || !binding || binding.detaching ||
                                       !binding.audio ||
                                       event.track.kind !== 'audio')
        return;
      const stream = event.streams && event.streams[0]
                         ? event.streams[0]
                         : new MediaStream([event.track]);
      const audio = binding.audio;
      audio.srcObject = stream;
      Promise.resolve(audio.play()).catch(() => {});
    };
    return 0;
  }
  catch(_) {
    peers.delete(peer_address);
    return -4;
  }
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_add_ice_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "pointer", "u32", "pointer", "u32", "pointer", "u32"], "i32",
    (peer_address, url, url_len, username, username_len, credential, credential_len) => {
        const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
        if (!entry)
          return -10;
        try {
          const server = {urls : UTF8ToString(url, url_len)};
          if (username_len)
            server.username = UTF8ToString(username, username_len);
          if (credential_len)
            server.credential = UTF8ToString(credential, credential_len);
          entry.iceServers.push(server);
          entry.pc.setConfiguration({
            ... entry.pc.getConfiguration(),
            iceServers : entry.iceServers.slice()
          });
          return 0;
        }
        catch(_) { return -4; }
      });
});
/* clang-format on */

// Completes op_id with the offer result; ICE gathering is capped so an
// unreachable STUN/TURN server cannot stall the offer forever.

/* clang-format off */
EM_JS(void, h2_web_webrtc_start_offer_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "u32", "u32"], "i32",
    (platform_address, peer_address, op_id) => {
  const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
  if (!entry)
    return -10;
  const complete = (result) =>
      Module['_h2_web_async_complete'](platform_address, op_id, result);
  (async () => {
    try {
      const pc = entry.pc;
      const offer = await pc.createOffer();
      if (Module['h2WebRtcPeers']?.get(peer_address) !== entry)
        return -10;
      await pc.setLocalDescription(offer);
      entry.watchDtlsErrors();
      if (Module['h2WebRtcPeers']?.get(peer_address) !== entry)
        return -10;
      if (pc.iceGatheringState !== 'complete') {
        await new Promise((resolve) => {
          const finish = () => {
            clearTimeout(timer);
            pc.removeEventListener('icegatheringstatechange', changed);
            entry.cancelOffer = null;
            resolve();
          };
          const changed = () => {
            if (pc.iceGatheringState === 'complete')
              finish();
          };
          const timer = setTimeout(() => {
            console.warn('Web WebRTC ICE gathering incomplete after ' +
                         `${Module['h2WebRtcIceGatherTimeoutMs'] || 10000} ms; ` +
                         'offering the candidates gathered so far');
            finish();
          }, Module['h2WebRtcIceGatherTimeoutMs'] || 10000);
          entry.cancelOffer = finish;
          pc.addEventListener('icegatheringstatechange', changed);
          changed();
        });
      }
      if (Module['h2WebRtcPeers']?.get(peer_address) !== entry)
        return -10;
      const sdp = entry.pc.localDescription ?.sdp;
      if (typeof sdp !== 'string')
        return -4;
      const length = lengthBytesUTF8(sdp);
      const buffer = _malloc(length + 1);
      if (!buffer)
        return -5;
      stringToUTF8(sdp, buffer, length + 1);
      Module['_h2_web_webrtc_local_sdp'](peer_address, 1, buffer, length);
      _free(buffer);
      return Module['h2WebRtcPeers'] ?.get(peer_address) === entry ? 0 : -10;
    }
    catch(_) {
      return Module['h2WebRtcPeers'] ?.get(peer_address) === entry ? -4 : -10;
    }
  })().then(complete);
  return 0;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_set_media_track_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "u32"], "i32",
    (peer_address, token) => {
        const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
        if (!entry)
          return -10;
        if (entry.binding || entry.pc.signalingState !== 'stable' ||
            entry.pc.localDescription)
          return -7;
        token = token >>> 0;
        const media = Module['h2WebRtcTracks'] ?.get(token);
        if (!media || (!media.stream && !media.audio))
          return -1;
        const owners = Module['h2WebRtcTrackOwners'] ||= new Map();
        if (owners.has(token))
          return -7;
        const binding = {
          token,
          stream : media.stream || null,
          audio : media.audio || null,
          senders : [],
          detaching : false
        };
        try {
          if (binding.audio &&
              (typeof binding.audio.play !== 'function' ||
               typeof binding.audio.pause !== 'function'))
            return -1;
          if (binding.stream) {
            const tracks = binding.stream.getAudioTracks();
            if (!tracks.length)
              return -1;
            for (const track of tracks)
              binding.senders.push(entry.pc.addTrack(track, binding.stream));
          } else {
            // Playback-only bindings still need an audio m-line in the offer.
            binding.senders.push(
                entry.pc.addTransceiver('audio', {direction : 'recvonly'})
                    .sender);
          }
          entry.binding = binding;
          owners.set(token, entry);
          return 0;
        }
        catch(_) {
          for (const sender of binding.senders) {
            try { entry.pc.removeTrack(sender); }
            catch(_) {}
          }
          return -4;
        }
      });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_unset_media_track_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "u32", "u32"], "i32",
    (platform_address, peer_address, op_id) => {
  const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
  if (!entry)
    return -10;
  const binding = entry.binding;
  if (!binding || binding.detaching)
    return -7;
  binding.detaching = true;
  const complete = (result) =>
      Module['_h2_web_async_complete'](platform_address, op_id, result);
  (async () => { try {
    if (binding.audio) {
      binding.audio.pause();
      binding.audio.srcObject = null;
    }
    // Even when one sender fails, wait for every detach already started. A
    // retry must not overlap promises from the previous attempt.
    const results = await Promise.allSettled(binding.senders.map(
        sender => Promise.resolve().then(() => sender.replaceTrack(null))));
    if (Module['h2WebRtcPeers']?.get(peer_address) !== entry)
      return -10;
    if (results.some(result => result.status === 'rejected')) {
      binding.detaching = false;
      return -4;
    }
    entry.binding = null;
    const owners = Module['h2WebRtcTrackOwners'];
    if (owners?.get(binding.token) === entry)
      owners.delete(binding.token);
    return 0;
  }
  catch(_) {
    binding.detaching = false;
    return Module['h2WebRtcPeers'] ?.get(peer_address) === entry ? -4 : -10;
  } })().then(complete);
  return 0;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_set_remote_sdp_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "u32", "u32", "i32", "pointer", "u32"], "i32",
    (platform_address, peer_address, op_id, type, sdp, sdp_len) => {
  const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
  if (!entry)
    return -10;
  if (type !== 2)
    return -7;
  const description = {type : 'answer', sdp : UTF8ToString(sdp, sdp_len)};
  entry.pc.setRemoteDescription(description).then(
      () => {
        if (Module['h2WebRtcPeers']?.get(peer_address) !== entry) return -10;
        entry.watchDtlsErrors();
        return 0;
      },
      () => Module['h2WebRtcPeers']?.get(peer_address) === entry ? -4 : -10)
      .then((result) => Module['_h2_web_async_complete'](
          platform_address, op_id, result));
  return 0;
});
});
/* clang-format on */

// Opus Tracks ride on browser-encoded RTP: a silent source keeps the browser
// Opus encoder producing one frame per packet time, and encoded transforms
// replace each outgoing payload with the Track's Opus packet and hand every
// incoming payload to the Track instead of the browser decoder. RTCRtpScript
// Transform (worker) is preferred; Chromium's createEncodedStreams is the
// fallback.

/* clang-format off */
EM_JS(void, h2_web_webrtc_set_opus_track_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], "i32",
    (peer_address) => {
  const entry = Module['h2WebRtcPeers']?.get(peer_address);
  if (!entry)
    return -10;
  if (entry.binding || entry.opus || entry.pc.signalingState !== 'stable' ||
      entry.pc.localDescription)
    return -7;
  const script = typeof globalThis.RTCRtpScriptTransform === 'function';
  const legacy = typeof globalThis.RTCRtpSender === 'function' &&
      typeof RTCRtpSender.prototype.createEncodedStreams === 'function';
  const AudioContext = globalThis.AudioContext || globalThis.webkitAudioContext;
  if ((!script && !legacy) || !AudioContext)
    return -3;
  // Runs in the transform context: sender payloads come from `port`,
  // receiver payloads go to it; received frames are not played by the browser.
  const run = function(readable, writable, side, port) {
    const queue = [];
    if (side === 'sender') {
      port.onmessage = (event) => {
        queue.push(event.data);
        if (queue.length > 50) queue.shift();
      };
    }
    readable.pipeThrough(new TransformStream({
      transform(frame, controller) {
        if (side === 'sender') {
          const packet = queue.shift();
          // The silent encoder is only a packet clock. It must not inject
          // unsolicited audio when the PAL caller has no packet queued.
          if (!packet) return;
          frame.data = packet;
          controller.enqueue(frame);
          port.postMessage(0);
        } else {
          const data = frame.data;
          port.postMessage(data, [data]);
        }
      }
    })).pipeTo(writable).catch(() => {});
  };
  try {
    const context = Module['h2WebAudioContext'] || new AudioContext();
    if (context.state !== 'running') {
      console.warn('Web WebRTC Opus Track: AudioContext is ' + context.state +
                   '; no uplink frames until a user gesture resumes it');
      context.resume().catch(() => {});
    }
    const source = context.createConstantSource();
    source.offset.value = 0;
    const destination = context.createMediaStreamDestination();
    source.connect(destination);
    source.start();
    const track = destination.stream.getAudioTracks()[0];
    const transceiver = entry.pc.addTransceiver(
        track, {direction: 'sendrecv', streams: [destination.stream]});
    const opus = {source, destination, track, transceiver, worker: null,
                  ports: [], rx: [], txQueued: 0, ownsContext:
                      context !== Module['h2WebAudioContext'], context};
    const wake = () => {
      if (Module['h2WebRtcPeers']?.get(peer_address) === entry)
        Module['_h2_web_webrtc_media_wake'](peer_address);
    };
    const sender = new MessageChannel();
    const receiver = new MessageChannel();
    sender.port1.onmessage = () => {
      opus.txQueued = Math.max(0, opus.txQueued - 1);
      wake();
    };
    receiver.port1.onmessage = (event) => {
      opus.rx.push(new Uint8Array(event.data));
      if (opus.rx.length > 100) opus.rx.shift();
      wake();
    };
    opus.txPort = sender.port1;
    opus.ports.push(sender.port1, receiver.port1);
    if (script) {
      const code = `const run = ${run.toString()};\n` +
          'onrtctransform = (event) => { const t = event.transformer; ' +
          'run(t.readable, t.writable, t.options.side, t.options.port); };';
      const url = URL.createObjectURL(new Blob([code],
                                               {type: 'text/javascript'}));
      opus.worker = new Worker(url);
      URL.revokeObjectURL(url);
      transceiver.sender.transform = new RTCRtpScriptTransform(
          opus.worker, {side: 'sender', port: sender.port2}, [sender.port2]);
      transceiver.receiver.transform = new RTCRtpScriptTransform(
          opus.worker, {side: 'receiver', port: receiver.port2},
          [receiver.port2]);
    } else {
      const out = transceiver.sender.createEncodedStreams();
      run(out.readable, out.writable, 'sender', sender.port2);
      const input = transceiver.receiver.createEncodedStreams();
      run(input.readable, input.writable, 'receiver', receiver.port2);
      opus.ports.push(sender.port2, receiver.port2);
    }
    opus.teardown = () => {
      if (entry.opus !== opus)
        return;
      entry.opus = null;
      for (const port of opus.ports) {
        port.onmessage = null;
        try { port.close(); } catch (_) {}
      }
      try { opus.transceiver.sender.replaceTrack(null).catch(() => {}); }
      catch (_) {}
      try { opus.source.stop(); } catch (_) {}
      opus.track.stop();
      opus.worker?.terminate();
      if (opus.ownsContext)
        opus.context.close().catch(() => {});
    };
    entry.opus = opus;
    return 0;
  } catch (error) {
    console.error('Web WebRTC Opus track setup failed', error);
    return -4;
  }
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_opus_teardown_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], null,
    (peer_address) => {
  Module['h2WebRtcPeers']?.get(peer_address)?.opus?.teardown();
});
});
/* clang-format on */

// Copies one received Opus payload; -9 when none is queued.

/* clang-format off */
EM_JS(void, h2_web_webrtc_opus_rx_take_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "pointer", "u32"], "i32",
    (peer_address, out, out_cap) => {
  const opus = Module['h2WebRtcPeers']?.get(peer_address)?.opus;
  if (!opus)
    return -10;
  const packet = opus.rx.shift();
  if (!packet)
    return -9;
  if (packet.byteLength > out_cap)
    return -13;
  HEAPU8.set(packet, out);
  return packet.byteLength;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_opus_tx_queued_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], "i32",
    (peer_address) => {
  const opus = Module['h2WebRtcPeers']?.get(peer_address)?.opus;
  return opus ? opus.txQueued : -10;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_opus_tx_push_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "pointer", "u32"], null,
    (peer_address, data, len) => {
  const opus = Module['h2WebRtcPeers']?.get(peer_address)?.opus;
  if (!opus)
    return;
  const buffer = HEAPU8.slice(data, data + len).buffer;
  opus.txPort.postMessage(buffer, [buffer]);
  ++opus.txQueued;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_channel_create_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "u32", "pointer", "u32", "i32", "u16", "i32", "i32", "i32"], "i32",
    (peer_address, channel_address, label, label_len, has_stream_id, stream_id, ordered, reliable, negotiated) => {
        const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
        if (!entry)
          return -10;
        try {
          const options = {ordered : !!ordered, negotiated : !!negotiated};
          if (has_stream_id)
            options.id = stream_id;
          if (!reliable)
            options.maxRetransmits = 0;
          const dc = entry.pc.createDataChannel(UTF8ToString(label, label_len),
                                                options);
          entry.bindChannel(channel_address, dc);
          return 0;
        }
        catch(error) {
          return error && error.name === 'OperationError' ? -13 : -4;
        }
      });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_channel_send_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "pointer", "u32", "i32"], "i32",
    (channel_address, data, len, is_text) => {
        const entry = Module['h2WebRtcChannels'] ?.get(channel_address);
        if (!entry)
          return -10;
        const dc = entry.dc;
        if (dc.readyState === 'connecting')
          return -9;
        if (dc.readyState !== 'open')
          return -10;
        const peer = Module['h2WebRtcPeers'] ?.get(entry.peerAddress);
        const negotiated = peer ?.pc.sctp ?.maxMessageSize || Infinity;
        if (Number(len) > Math.min(1024 * 1024, negotiated))
          return -13;
        if (dc.bufferedAmount + Number(len) > 1024 * 1024)
          return -9;
        try {
          const bytes = HEAPU8.slice(data, data + Number(len));
          dc.send(is_text ? new TextDecoder().decode(bytes) : bytes);
          return 0;
        }
        catch(_) { return -4; }
      });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_channel_close_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], null,
    (channel_address) => {
  const channels = Module['h2WebRtcChannels'];
  const entry = channels ?.get(channel_address);
  if (!entry)
    return;
  channels.delete(channel_address);
  entry.dc.onopen = entry.dc.onclose = entry.dc.onerror = entry.dc.onmessage =
      null;
  entry.dc.onbufferedamountlow = null;
  try { entry.dc.close(); }
  catch(_) {}
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_webrtc_peer_close_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], null,
    (peer_address) => {
  const peers = Module['h2WebRtcPeers'];
  const entry = peers ?.get(peer_address);
  if (!entry)
    return;
  peers.delete(peer_address);
  const channels = Module['h2WebRtcChannels'];
  if (channels) {
    for (const[address, channel] of channels) {
      if (channel.peerAddress !== peer_address)
        continue;
      channels.delete(address);
      channel.dc.onopen = channel.dc.onclose = channel.dc.onerror =
          channel.dc.onmessage = null;
      channel.dc.onbufferedamountlow = null;
      try { channel.dc.close(); }
      catch(_) {}
    }
  }
  entry.pc.onconnectionstatechange = entry.pc.ondatachannel = entry.pc.ontrack =
      null;
  for (const [transport, listener] of entry.dtlsErrors)
    transport.removeEventListener('error', listener);
  entry.dtlsErrors.clear();
  if (entry.cancelOffer)
    entry.cancelOffer();
  entry.opus?.teardown();
  if (entry.binding) {
    const binding = entry.binding;
    binding.detaching = true;
    if (binding.audio) {
      binding.audio.pause();
      binding.audio.srcObject = null;
    }
    const owners = Module['h2WebRtcTrackOwners'];
    if (owners?.get(binding.token) === entry)
      owners.delete(binding.token);
    entry.binding = null;
  }
  try { entry.pc.close(); }
  catch(_) {}
});
});
/* clang-format on */

static void h2_web_webrtc_unlink_channel(h2_pal_webrtc_channel_t *channel) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_channel_t **cursor = &channel->peer->channels;
  while (*cursor != NULL && *cursor != channel)
    cursor = &(*cursor)->next;
  if (*cursor == channel)
    *cursor = channel->next;
}

static void h2_web_webrtc_free_channel(h2_pal_webrtc_channel_t *channel) {
  H2_WEB_STATE_GUARD();
  h2_pal_mem_api_t memory = channel->peer->allocator;
  h2_pal_mem_free(&memory, channel->label);
  h2_pal_mem_free(&memory, channel);
}

static void h2_web_webrtc_peer_close_now(h2_pal_webrtc_peer_t *peer) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t *owner = peer->owner;
  (void)h2_web_main_call(h2_web_webrtc_peer_close_js,
                         (const void *[]){&(uintptr_t){(uintptr_t)peer}});
  while (peer->event_head != NULL) {
    h2_web_webrtc_event_t *event = peer->event_head;
    peer->event_head = event->next;
    h2_pal_webrtc_event_t public_event = event->event;
    public_event._private = event;
    public_event._release = h2_web_webrtc_event_release;
    h2_web_webrtc_event_release(&public_event);
  }
  while (peer->channels != NULL) {
    h2_pal_webrtc_channel_t *channel = peer->channels;
    peer->channels = channel->next;
    h2_web_webrtc_free_channel(channel);
  }
  h2_pal_webrtc_peer_t **cursor = &owner->webrtc_peers;
  while (*cursor != NULL && *cursor != peer)
    cursor = &(*cursor)->next;
  if (*cursor == peer)
    *cursor = peer->next;
  h2_pal_mem_api_t memory = peer->allocator;
  h2_pal_mem_free(&memory, peer);
}

static bool h2_web_webrtc_peer_end_async(h2_pal_webrtc_peer_t *peer) {
  H2_WEB_STATE_GUARD();
  if (--peer->async_calls == 0u && peer->close_pending) {
    h2_web_webrtc_peer_close_now(peer);
    return true;
  }
  return false;
}

EMSCRIPTEN_KEEPALIVE void h2_web_webrtc_peer_state(uintptr_t peer_address,
                                                   int state) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  if (peer == NULL || peer->closed || state < H2_PAL_WEBRTC_PEER_NEW ||
      state > H2_PAL_WEBRTC_PEER_CLOSED || peer->state == state)
    return;
  peer->state = (h2_pal_webrtc_peer_state_t)state;
  (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_PEER_STATE, NULL, 0,
                              peer->state, 0, NULL, 0u, 0);
}

EMSCRIPTEN_KEEPALIVE void h2_web_webrtc_local_sdp(uintptr_t peer_address,
                                                  int type, const char *sdp,
                                                  size_t sdp_len) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  if (peer == NULL || peer->closed)
    return;
  (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_LOCAL_SDP, NULL, 0, 0,
                              (h2_pal_webrtc_sdp_type_t)type, sdp, sdp_len, 0);
}

static h2_pal_webrtc_channel_t *
h2_web_webrtc_new_channel(h2_pal_webrtc_peer_t *peer, const char *label,
                          size_t label_len, uint16_t stream_id,
                          int has_stream_id, int ordered, int reliable,
                          int negotiated) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_channel_t *channel = h2_pal_mem_alloc(&peer->allocator, sizeof(*channel));
  if (channel == NULL)
    return NULL;
  memset(channel, 0, sizeof(*channel));
  channel->label = h2_web_webrtc_copy_string(&peer->allocator, label, label_len);
  if (channel->label == NULL) {
    h2_pal_mem_free(&peer->allocator, channel);
    return NULL;
  }
  channel->peer = peer;
  channel->info = (h2_pal_webrtc_channel_info_t){
      .label = {.data = channel->label, .len = label_len},
      .stream_id = stream_id,
      .has_stream_id = has_stream_id != 0,
      .ordered = ordered != 0,
      .reliable = reliable != 0,
      .negotiated = negotiated != 0,
  };
  channel->next = peer->channels;
  peer->channels = channel;
  return channel;
}

EMSCRIPTEN_KEEPALIVE uintptr_t h2_web_webrtc_remote_channel(
    uintptr_t peer_address, const char *label, size_t label_len,
    uint16_t stream_id, int has_stream_id, int ordered, int reliable) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  if (peer == NULL || peer->closed || (label == NULL && label_len != 0u))
    return 0u;
  if (peer->event_error != H2_PAL_OK)
    return 0u;
  h2_pal_webrtc_channel_t *channel = h2_web_webrtc_new_channel(
      peer, label, label_len, stream_id, has_stream_id, ordered, reliable, 0);
  if (channel == NULL)
    h2_web_webrtc_fail(peer_address, H2_PAL_ERR_NO_MEMORY);
  return (uintptr_t)channel;
}

EMSCRIPTEN_KEEPALIVE void
h2_web_webrtc_channel_metadata(uintptr_t peer_address,
                               uintptr_t channel_address, uint16_t stream_id,
                               int has_stream_id) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  h2_pal_webrtc_channel_t *channel =
      h2_web_webrtc_find_channel(peer, channel_address);
  if (channel == NULL || channel->terminal)
    return;
  channel->info.stream_id = stream_id;
  channel->info.has_stream_id = has_stream_id != 0;
}

EMSCRIPTEN_KEEPALIVE void h2_web_webrtc_channel_state(uintptr_t peer_address,
                                                      uintptr_t channel_address,
                                                      int state) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  h2_pal_webrtc_channel_t *channel =
      h2_web_webrtc_find_channel(peer, channel_address);
  if (channel == NULL || channel->terminal || peer->closed ||
      state < H2_PAL_WEBRTC_CHANNEL_OPEN || state > H2_PAL_WEBRTC_CHANNEL_ERROR)
    return;
  channel->terminal = state != H2_PAL_WEBRTC_CHANNEL_OPEN;
  (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, channel,
                              (h2_pal_webrtc_channel_state_t)state, 0, 0, NULL,
                              0u, 0);
}

EMSCRIPTEN_KEEPALIVE void
h2_web_webrtc_channel_message(uintptr_t peer_address, uintptr_t channel_address,
                              const uint8_t *data, size_t len, int is_text) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  h2_pal_webrtc_channel_t *channel =
      h2_web_webrtc_find_channel(peer, channel_address);
  if (channel == NULL || channel->terminal || peer->closed)
    return;
  (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE,
                              channel, 0, 0, 0, data, len, is_text);
}

EMSCRIPTEN_KEEPALIVE void
h2_web_webrtc_channel_writable(uintptr_t peer_address,
                               uintptr_t channel_address) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  h2_pal_webrtc_channel_t *channel =
      h2_web_webrtc_find_channel(peer, channel_address);
  if (channel == NULL || channel->terminal || peer->closed)
    return;
  (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_WRITABLE, channel, 0, 0,
                              0, NULL, 0u, 0);
}

static h2_pal_result_t
h2_web_webrtc_peer_create_with_config(void *user, const h2_pal_webrtc_peer_config_t *config,
                                        h2_pal_webrtc_peer_t **out_peer) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t *platform = user;
  if (platform == NULL || out_peer == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  *out_peer = NULL;
  const h2_pal_mem_api_t *memory = config && config->allocator ? config->allocator : h2_web_platform_mem_api();
  if (!memory->vtable || !memory->vtable->alloc || !memory->vtable->free)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_webrtc_peer_t *peer = h2_pal_mem_alloc(memory, sizeof(*peer));
  if (peer == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(peer, 0, sizeof(*peer));
  peer->allocator = *memory;
  peer->owner = platform;
  peer->state = H2_PAL_WEBRTC_PEER_NEW;
  const h2_pal_result_t result =
      (h2_pal_result_t)((int)h2_web_main_call(
                            h2_web_webrtc_peer_create_js,
                            (const void *[]){&(uintptr_t){(uintptr_t)peer}})
                            .i32);
  if (result != H2_PAL_OK) {
    h2_pal_mem_free(memory, peer);
    return result;
  }
  peer->next = platform->webrtc_peers;
  platform->webrtc_peers = peer;
  *out_peer = peer;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_webrtc_peer_add_ice_server(h2_pal_webrtc_peer_t *peer,
                                  const h2_pal_webrtc_ice_server_t *server) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || server == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (peer->offer_started)
    return H2_PAL_ERR_INVALID_STATE;
  return (h2_pal_result_t)((int)h2_web_main_call(
                               h2_web_webrtc_add_ice_js,
                               (const void *[]){
                                   &(uintptr_t){(uintptr_t)peer},
                                   &(const char *){server->url.data},
                                   &(size_t){server->url.len},
                                   &(const char *){server->username.data},
                                   &(size_t){server->username.len},
                                   &(const char *){server->credential.data},
                                   &(size_t){server->credential.len}})
                               .i32);
}

typedef struct h2_web_webrtc_control {
  struct h2_web_webrtc_control *next;
  h2_web_async_t op;
} h2_web_webrtc_control_t;

/* Finish one browser Promise started with control->op; peer_close ends it. */
static h2_pal_result_t
h2_web_webrtc_control_finish(h2_pal_webrtc_peer_t *peer,
                             h2_web_webrtc_control_t *control,
                             h2_pal_result_t started) {
  H2_WEB_STATE_GUARD();
  control->next = peer->controls;
  peer->controls = control;
  const h2_pal_result_t result = (h2_pal_result_t)h2_web_async_finish(
      peer->owner, &control->op, started);
  h2_web_webrtc_control_t **cursor = &peer->controls;
  while (*cursor != control)
    cursor = &(*cursor)->next;
  *cursor = control->next;
  return result;
}

static h2_pal_result_t h2_web_webrtc_set_opus_track(h2_pal_webrtc_peer_t *peer,
                                                  h2_pal_webrtc_track_t *track);

static h2_pal_result_t
h2_web_webrtc_peer_start_offer(h2_pal_webrtc_peer_t *peer) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (!h2_web_platform_fake_network_available(peer->owner))
    return H2_PAL_ERR_UNAVAILABLE;
  if (peer->offer_started)
    return H2_PAL_ERR_INVALID_STATE;
  if (peer->media_track == NULL && !peer->opus_mode) {
    h2_pal_result_t media = h2_web_webrtc_set_opus_track(peer, NULL);
    // Legacy hosts without encoded transforms can still negotiate data-only
    // peers. Raw Opus remains explicitly unsupported on those hosts.
    if (media != H2_PAL_OK && media != H2_PAL_ERR_UNSUPPORTED) return media;
  }
  peer->offer_started = true;
  peer->async_calls++;
  h2_web_webrtc_control_t control;
  h2_web_async_begin(peer->owner, &control.op);
  h2_pal_result_t result = (h2_pal_result_t)((int)h2_web_main_call(
                            h2_web_webrtc_start_offer_js,
                            (const void *[]){
                                &(uintptr_t){(uintptr_t)peer->owner},
                                &(uintptr_t){(uintptr_t)peer},
                                &(uint32_t){control.op.id}})
                            .i32);
  result = h2_web_webrtc_control_finish(peer, &control, result);
  if (peer->closed && result == H2_PAL_OK)
    result = H2_PAL_ERR_CLOSED;
  if (result == H2_PAL_OK && peer->event_error != H2_PAL_OK)
    result = peer->event_error;
  (void)h2_web_webrtc_peer_end_async(peer);
  return result;
}

static h2_pal_result_t
h2_web_webrtc_peer_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                                  h2_pal_webrtc_sdp_type_t type,
                                  h2_pal_webrtc_str_t sdp) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  peer->async_calls++;
  h2_web_webrtc_control_t control;
  h2_web_async_begin(peer->owner, &control.op);
  h2_pal_result_t rc = (h2_pal_result_t)((int)h2_web_main_call(
                            h2_web_webrtc_set_remote_sdp_js,
                            (const void *[]){
                                &(uintptr_t){(uintptr_t)peer->owner},
                                &(uintptr_t){(uintptr_t)peer},
                                &(uint32_t){control.op.id}, &(int){type},
                                &(const char *){sdp.data}, &(size_t){sdp.len}})
                            .i32);
  rc = h2_web_webrtc_control_finish(peer, &control, rc);
  if (peer->closed)
    rc = H2_PAL_ERR_CLOSED;
  (void)h2_web_webrtc_peer_end_async(peer);
  return rc;
}

static h2_pal_result_t h2_web_webrtc_peer_create_data_channel(
    h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_channel_config_t *config,
    h2_pal_webrtc_channel_t **out_channel) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || config == NULL || out_channel == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  *out_channel = NULL;
  // Never silently replace a caller's fixed ID with a browser-assigned one.
  if (config->has_stream_id && !config->negotiated)
    return H2_PAL_ERR_UNSUPPORTED;
  if (peer->event_error != H2_PAL_OK)
    return peer->event_error;
  h2_pal_webrtc_channel_t *channel = h2_web_webrtc_new_channel(
      peer, config->label.data, config->label.len, config->stream_id,
      config->has_stream_id, config->ordered, config->reliable, config->negotiated);
  if (channel == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  const h2_pal_result_t result =
      (h2_pal_result_t)((int)h2_web_main_call(
                            h2_web_webrtc_channel_create_js,
                            (const void *[]){
                                &(uintptr_t){(uintptr_t)peer},
                                &(uintptr_t){(uintptr_t)channel},
                                &(const char *){config->label.data},
                                &(size_t){config->label.len},
                                &(int){config->has_stream_id},
                                &(uint16_t){config->stream_id},
                                &(int){config->ordered},
                                &(int){config->reliable},
                                &(int){config->negotiated}})
                            .i32);
  if (result != H2_PAL_OK) {
    h2_web_webrtc_unlink_channel(channel);
    h2_web_webrtc_free_channel(channel);
    return result;
  }
  *out_channel = channel;
  return H2_PAL_OK;
}

// Largest Opus packet accepted from a Track or the network (RFC 6716 bound).
#define H2_WEB_WEBRTC_OPUS_MAX 1275u
// Opus packets queued ahead of the browser's 20 ms packet clock.
#define H2_WEB_WEBRTC_OPUS_TX_AHEAD 3
#define H2_WEB_WEBRTC_MEDIA_PERIOD_MS 10u

EMSCRIPTEN_KEEPALIVE void h2_web_webrtc_media_wake(uintptr_t peer_address) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  if (peer != NULL && peer->media_op != NULL)
    h2_web_async_signal(peer->owner, peer->media_op, H2_PAL_OK);
  if (peer != NULL && !peer->closed && peer->opus_send_blocked) {
    peer->opus_send_blocked = false;
    (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_WRITABLE, NULL,
        0, 0, 0, NULL, 0u, 0);
  }
}

/*
 * Moves Opus between the Track and the browser. Track callbacks run here, on
 * a task as the Track contract requires, and never from browser callbacks.
 */
static void h2_web_webrtc_media_entry(void *user) {
  H2_WEB_STATE_GUARD();
  h2_pal_webrtc_peer_t *peer = user;
  uint8_t packet[H2_WEB_WEBRTC_OPUS_MAX];
  int pending_length = -1;
  while (!peer->media_stop) {
    for (unsigned burst = 0; burst < 8u && !peer->media_stop; ++burst) {
      if (peer->track_detaching) break;
      h2_pal_webrtc_track_t *track = peer->media_track;
      if (pending_length < 0) {
        pending_length = (int)h2_web_main_call(h2_web_webrtc_opus_rx_take_js,
            (const void *[]){&(uintptr_t){(uintptr_t)peer},
                             &(uint8_t *){packet}, &(size_t){sizeof(packet)}}).i32;
      }
      /* The bridge releases the state lock. Close or detach may have finished
       * while JS ran, so re-check before borrowing any Track context. */
      if (peer->media_stop || peer->track_detaching ||
          peer->media_track != track) break;
      if (pending_length < 0) break;
      h2_pal_result_t rc = H2_PAL_OK;
      if (track != NULL) {
        peer->media_in_callback = true;
        unsigned depth = h2_web_state_pause();
        rc = track->vtable->write(track->user, packet, (size_t)pending_length);
        h2_web_state_resume(depth);
        peer->media_in_callback = false;
      } else {
        (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_OPUS_FRAME,
            NULL, 0, 0, 0, packet, (size_t)pending_length, 0);
      }
      if (rc == H2_PAL_ERR_WOULD_BLOCK) break;
      pending_length = -1;
      if (rc != H2_PAL_OK) {
        h2_web_webrtc_fail((uintptr_t)peer, rc);
        break;
      }
    }
    uint8_t outgoing[H2_WEB_WEBRTC_OPUS_MAX];
    while (!peer->media_stop && !peer->track_detaching &&
           peer->media_track != NULL) {
      h2_pal_webrtc_track_t *track = peer->media_track;
      int queued = (int)h2_web_main_call(h2_web_webrtc_opus_tx_queued_js,
            (const void *[]){&(uintptr_t){(uintptr_t)peer}}).i32;
      if (peer->media_stop || peer->track_detaching ||
          peer->media_track != track || queued >= H2_WEB_WEBRTC_OPUS_TX_AHEAD)
        break;
      size_t length = 0u;
      peer->media_in_callback = true;
      unsigned depth = h2_web_state_pause();
      const h2_pal_result_t rc = track->vtable->read(
          track->user, outgoing, sizeof(outgoing), &length);
      h2_web_state_resume(depth);
      peer->media_in_callback = false;
      if (rc != H2_PAL_OK || length == 0u || length > sizeof(outgoing) ||
          peer->media_stop) break;
      (void)h2_web_main_call(h2_web_webrtc_opus_tx_push_js,
          (const void *[]){&(uintptr_t){(uintptr_t)peer},
                           &(const uint8_t *){outgoing}, &(size_t){length}});
    }
    if (peer->media_stop)
      break;
    h2_web_async_t op;
    h2_web_async_begin(peer->owner, &op);
    peer->media_op = &op;
    const h2_pal_result_t wait = h2_web_async_wait(
        peer->owner, &op, H2_WEB_WEBRTC_MEDIA_PERIOD_MS);
    peer->media_op = NULL;
    h2_web_async_end(peer->owner, &op);
    if (wait == H2_PAL_ERR_CLOSED)
      break;
  }
  // The platform pump joins the finished task; the peer may be freed here.
  h2_web_platform_t *owner = peer->owner;
  h2_web_webrtc_zombie_t *zombie = malloc(sizeof(*zombie));
  if (zombie != NULL) {
    *zombie = (h2_web_webrtc_zombie_t){
        .next = owner->webrtc_zombies,
        .task = peer->media_task,
    };
    owner->webrtc_zombies = zombie;
  }
  peer->media_task = NULL;
  (void)h2_web_webrtc_peer_end_async(peer);
}

static void h2_web_webrtc_media_stop(h2_pal_webrtc_peer_t *peer) {
  H2_WEB_STATE_GUARD();
  if (!peer->opus_mode)
    return;
  peer->media_stop = true;
  (void)h2_web_main_call(h2_web_webrtc_opus_teardown_js,
                         (const void *[]){&(uintptr_t){(uintptr_t)peer}});
  h2_web_webrtc_media_wake((uintptr_t)peer);
}

static h2_pal_result_t
h2_web_webrtc_set_opus_track(h2_pal_webrtc_peer_t *peer,
                             h2_pal_webrtc_track_t *track) {
  H2_WEB_STATE_GUARD();
  if (track != NULL && (track->vtable == NULL || track->vtable->read == NULL ||
      track->vtable->write == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  // The media task of a previous Track may still be winding down.
  if (peer->media_task != NULL)
    return H2_PAL_ERR_BUSY;
  h2_pal_result_t rc =
      (h2_pal_result_t)((int)h2_web_main_call(
                            h2_web_webrtc_set_opus_track_js,
                            (const void *[]){&(uintptr_t){(uintptr_t)peer}})
                            .i32);
  if (rc != H2_PAL_OK)
    return rc;
  peer->media_track = track;
  peer->opus_mode = true;
  peer->media_stop = false;
  peer->async_calls++;
  const h2_pal_task_options_t options = {.name = "webrtc/media"};
  rc = h2_pal_task_start(h2_web_platform_task_api(peer->owner), &options,
                         h2_web_webrtc_media_entry, peer, &peer->media_task);
  if (rc != H2_PAL_OK) {
    peer->async_calls--;
    peer->opus_mode = false;
    peer->media_track = NULL;
    (void)h2_web_main_call(h2_web_webrtc_opus_teardown_js,
                           (const void *[]){&(uintptr_t){(uintptr_t)peer}});
  }
  return rc;
}

/* Wait until no callback can still access the borrowed Track context. */
static h2_pal_result_t
h2_web_webrtc_media_quiesce(h2_pal_webrtc_peer_t *peer) {
  H2_WEB_STATE_GUARD();
  while (peer->media_in_callback) {
    const h2_pal_result_t slept =
        h2_pal_time_sleep_ms(h2_web_platform_time_api(peer->owner), 1u);
    if (slept == H2_PAL_ERR_INVALID_STATE)
      return H2_PAL_ERR_BUSY;
    if (slept != H2_PAL_OK)
      return slept == H2_PAL_EXIT ? H2_PAL_ERR_CLOSED : slept;
  }
  return H2_PAL_OK;
}

void h2_web_platform_webrtc_reap(h2_web_platform_t *platform) {
  H2_WEB_STATE_GUARD();
  while (platform->webrtc_zombies != NULL) {
    h2_web_webrtc_zombie_t *zombie = platform->webrtc_zombies;
    platform->webrtc_zombies = zombie->next;
    // The finishing Worker may still need the state lock during its final
    // cleanup. Reserve this join before releasing the lock so another reaper
    // cannot consume the same handle.
    unsigned depth = h2_web_state_pause();
    h2_pal_result_t rc =
        h2_pal_task_join(h2_web_platform_task_api(platform), zombie->task);
    h2_web_state_resume(depth);
    if (rc != H2_PAL_OK) {
      zombie->next = platform->webrtc_zombies;
      platform->webrtc_zombies = zombie;
      return;
    }
    free(zombie);
  }
}

static h2_pal_result_t
h2_web_webrtc_peer_set_track(h2_pal_webrtc_peer_t *peer,
                             h2_pal_webrtc_track_t *track) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (peer->offer_started || peer->media_track != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  if (track->native_handle == NULL) {
    if (peer->opus_mode) {
      if (!track->vtable || !track->vtable->read || !track->vtable->write)
        return H2_PAL_ERR_INVALID_ARG;
      peer->media_track = track;
      return H2_PAL_OK;
    }
    return h2_web_webrtc_set_opus_track(peer, track);
  }
  if (peer->opus_mode) return H2_PAL_ERR_INVALID_STATE;
  h2_pal_result_t rc = (h2_pal_result_t)((int)h2_web_main_call(
                            h2_web_webrtc_set_media_track_js,
                            (const void *[]){
                                &(uintptr_t){(uintptr_t)peer},
                                &(uintptr_t){(uintptr_t)track->native_handle}})
                            .i32);
  if (rc == H2_PAL_OK)
    peer->media_track = track;
  return rc;
}

static h2_pal_result_t
h2_web_webrtc_peer_unset_track(h2_pal_webrtc_peer_t *peer,
                               h2_pal_webrtc_track_t *track) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (peer->media_track != track || peer->track_detaching)
    return H2_PAL_ERR_INVALID_STATE;
  if (peer->opus_mode) {
    peer->track_detaching = true;
    const h2_pal_result_t quiet = h2_web_webrtc_media_quiesce(peer);
    if (quiet != H2_PAL_OK) { peer->track_detaching = false; return quiet; }
    // Keep the negotiated raw Opus transport; subsequent packets are owned
    // events and peer_send_opus remains available after Track detach.
    peer->media_track = NULL;
    peer->track_detaching = false;
    return H2_PAL_OK;
  }
  peer->track_detaching = true;
  peer->async_calls++;
  h2_web_webrtc_control_t control;
  h2_web_async_begin(peer->owner, &control.op);
  h2_pal_result_t rc = (h2_pal_result_t)((int)h2_web_main_call(
                            h2_web_webrtc_unset_media_track_js,
                            (const void *[]){
                                &(uintptr_t){(uintptr_t)peer->owner},
                                &(uintptr_t){(uintptr_t)peer},
                                &(uint32_t){control.op.id}})
                            .i32);
  rc = h2_web_webrtc_control_finish(peer, &control, rc);
  if (peer->closed)
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK)
    peer->media_track = NULL;
  peer->track_detaching = false;
  (void)h2_web_webrtc_peer_end_async(peer);
  return rc;
}

static h2_pal_result_t h2_web_webrtc_peer_poll(h2_pal_webrtc_peer_t *peer,
                                               int timeout_ms,
                                               h2_pal_webrtc_event_t *event) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (timeout_ms < 0)
    return H2_PAL_ERR_INVALID_ARG;
  if (peer->poll_waiting)
    return H2_PAL_ERR_BUSY;
  h2_pal_result_t result = h2_web_webrtc_dequeue(peer, event);
  if (result != H2_PAL_ERR_WOULD_BLOCK || timeout_ms == 0)
    return result;
  peer->poll_waiting = true;
  peer->async_calls++;
  h2_web_async_t op;
  h2_web_async_begin(peer->owner, &op);
  peer->poll_op = &op;
  // Tasks yield here; other tasks, timers and the root keep running.
  result = h2_web_async_wait(peer->owner, &op, (uint32_t)timeout_ms);
  peer->poll_op = NULL;
  h2_web_async_end(peer->owner, &op);
  if (peer->closed)
    result = H2_PAL_ERR_CLOSED;
  else if (result == H2_PAL_OK)
    result = h2_web_webrtc_dequeue(peer, event);
  if (result == H2_PAL_ERR_WOULD_BLOCK)
    result = H2_PAL_ERR_TIMEOUT;
  peer->poll_waiting = false;
  (void)h2_web_webrtc_peer_end_async(peer);
  return result;
}

static h2_pal_result_t h2_web_webrtc_peer_send_opus(h2_pal_webrtc_peer_t *peer,
                                                    const uint8_t *opus,
                                                    size_t opus_len) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || peer->closed) return H2_PAL_ERR_CLOSED;
  if (opus == NULL || !opus_len || opus_len > H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE)
    return H2_PAL_ERR_INVALID_ARG;
  if (peer->state != H2_PAL_WEBRTC_PEER_CONNECTED)
    return H2_PAL_ERR_INVALID_STATE;
  if (!peer->opus_mode) return H2_PAL_ERR_UNSUPPORTED;
  if (!h2_web_platform_fake_network_available(peer->owner)) return H2_PAL_ERR_UNAVAILABLE;
  int queued = (int)h2_web_main_call(h2_web_webrtc_opus_tx_queued_js,
      (const void *[]){&(uintptr_t){(uintptr_t)peer}}).i32;
  if (queued < 0) return H2_PAL_ERR_CLOSED;
  if (queued >= H2_WEB_WEBRTC_OPUS_TX_AHEAD) {
    peer->opus_send_blocked = true;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  (void)h2_web_main_call(h2_web_webrtc_opus_tx_push_js,
      (const void *[]){&(uintptr_t){(uintptr_t)peer},
                       &(const uint8_t *){opus}, &(size_t){opus_len}});
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_webrtc_channel_send(h2_pal_webrtc_channel_t *channel,
                           const uint8_t *data, size_t len, int is_text) {
  H2_WEB_STATE_GUARD();
  if (channel == NULL || channel->terminal)
    return H2_PAL_ERR_CLOSED;
  if (!h2_web_platform_fake_network_available(channel->peer->owner)) return H2_PAL_ERR_UNAVAILABLE;
  if (channel->peer->event_error != H2_PAL_OK)
    return channel->peer->event_error;
  return (h2_pal_result_t)((int)h2_web_main_call(
                           h2_web_webrtc_channel_send_js,
                           (const void *[]){&(uintptr_t){(uintptr_t)channel},
                                            &(const uint8_t *){data},
                                            &(size_t){len}, &(int){is_text}})
                           .i32);
}

static void h2_web_webrtc_channel_close(h2_pal_webrtc_channel_t *channel) {
  H2_WEB_STATE_GUARD();
  if (channel == NULL || channel->terminal)
    return;
  channel->terminal = true;
  (void)h2_web_main_call(h2_web_webrtc_channel_close_js,
                         (const void *[]){&(uintptr_t){(uintptr_t)channel}});
  (void)h2_web_webrtc_enqueue(channel->peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE,
                              channel, H2_PAL_WEBRTC_CHANNEL_CLOSED, 0, 0, NULL,
                              0u, 0);
}

static void h2_web_webrtc_peer_close(h2_pal_webrtc_peer_t *peer) {
  H2_WEB_STATE_GUARD();
  if (peer == NULL || peer->closed)
    return;
  peer->closed = true;
  // Track ownership ends synchronously at close, even though task joining
  // and browser Promise cleanup may complete later.
  peer->media_stop = true;
  while (peer->media_in_callback) {
    unsigned depth = h2_web_state_pause();
    h2_web_worker_sleep(1u);
    h2_web_state_resume(depth);
  }
  // Stop browser callbacks and cancel ICE waiting immediately; only the C
  // allocation waits for outstanding Worker calls to return.
  h2_web_webrtc_media_stop(peer);
  (void)h2_web_main_call(h2_web_webrtc_peer_close_js,
                         (const void *[]){&(uintptr_t){(uintptr_t)peer}});
  h2_web_webrtc_wake(peer);
  for (h2_web_webrtc_control_t *control = peer->controls; control != NULL;
       control = control->next)
    h2_web_async_signal(peer->owner, &control->op, H2_PAL_ERR_CLOSED);
  peer->media_track = NULL;
  if (peer->async_calls != 0u) {
    peer->close_pending = true;
    return;
  }
  h2_web_webrtc_peer_close_now(peer);
}

static h2_pal_result_t h2_web_webrtc_peer_create(void *user, h2_pal_webrtc_peer_t **out) {
  return h2_web_webrtc_peer_create_with_config(user, NULL, out);
}

static const h2_pal_webrtc_vtable_t h2_web_webrtc_vtable = {
    .peer_create = h2_web_webrtc_peer_create,
    .peer_create_with_config = h2_web_webrtc_peer_create_with_config,
    .peer_add_ice_server = h2_web_webrtc_peer_add_ice_server,
    .peer_start_offer = h2_web_webrtc_peer_start_offer,
    .peer_set_remote_sdp = h2_web_webrtc_peer_set_remote_sdp,
    .peer_create_data_channel = h2_web_webrtc_peer_create_data_channel,
    .peer_set_track = h2_web_webrtc_peer_set_track,
    .peer_unset_track = h2_web_webrtc_peer_unset_track,
    .peer_poll = h2_web_webrtc_peer_poll,
    .peer_send_opus = h2_web_webrtc_peer_send_opus,
    .channel_send = h2_web_webrtc_channel_send,
    .channel_close = h2_web_webrtc_channel_close,
    .peer_close = h2_web_webrtc_peer_close,
};

void h2_web_platform_webrtc_init(h2_web_platform_t *platform) {
  H2_WEB_STATE_GUARD();
  platform->webrtc_api = (h2_pal_webrtc_api_t){
      .user = platform,
      .vtable = &h2_web_webrtc_vtable,
  };
}

void h2_web_platform_webrtc_deinit(h2_web_platform_t *platform) {
  H2_WEB_STATE_GUARD();
  while (platform->webrtc_peers != NULL) {
    h2_pal_webrtc_peer_t *peer = platform->webrtc_peers;
    peer->closed = true;
    h2_web_webrtc_peer_close_now(peer);
  }
}

bool h2_web_platform_webrtc_busy(h2_web_platform_t *platform) {
  H2_WEB_STATE_GUARD();
  for (h2_pal_webrtc_peer_t *peer = platform->webrtc_peers; peer != NULL;
       peer = peer->next)
    if (peer->async_calls != 0u)
      return true;
  return false;
}
