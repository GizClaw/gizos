#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Embedded JS operators are not understood by the C formatter.
// clang-format off

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
} h2_web_webrtc_event_t;

static char *h2_web_webrtc_copy_string(const char *data, size_t len);

struct h2_pal_webrtc_peer {
  h2_web_platform_t *owner;
  h2_pal_webrtc_peer_t *next;
  h2_web_webrtc_event_t *event_head;
  h2_web_webrtc_event_t *event_tail;
  h2_pal_webrtc_channel_t *channels;
  size_t event_count;
  size_t event_bytes;
  h2_pal_result_t event_error;
  bool error_reported;
  bool poll_waiting;
  h2_pal_webrtc_peer_state_t state;
  h2_pal_webrtc_track_t *media_track;
  bool offer_started;
  bool track_detaching;
  unsigned async_calls;
  bool closed;
  bool close_pending;
};

EM_JS(void, h2_web_webrtc_wake_js, (uintptr_t address),
      { Module['h2WebRtcPeers'] ?.get(address) ?.wakePoll ?.(0); });

EMSCRIPTEN_KEEPALIVE void h2_web_webrtc_fail(uintptr_t address, int error) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)address;
  if (peer == NULL || peer->closed || peer->event_error != H2_PAL_OK)
    return;
  peer->event_error = (h2_pal_result_t)error;
  h2_web_webrtc_wake_js(address);
}

static void h2_web_webrtc_event_release(h2_pal_webrtc_event_t *event) {
  if (event == NULL || event->_private == NULL)
    return;
  h2_web_webrtc_event_t *node = event->_private;
  free(node->label);
  free(node->payload);
  free(node);
  memset(event, 0, sizeof(*event));
}

static int h2_web_webrtc_enqueue(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_event_kind_t kind,
    h2_pal_webrtc_channel_t *channel, h2_pal_webrtc_channel_state_t state,
    h2_pal_webrtc_peer_state_t peer_state, h2_pal_webrtc_sdp_type_t sdp_type,
    const void *payload, size_t payload_len, int is_text) {
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
  h2_web_webrtc_event_t *node = calloc(1u, sizeof(*node));
  if (node == NULL) {
    h2_web_webrtc_fail((uintptr_t)peer, H2_PAL_ERR_NO_MEMORY);
    return 0;
  }
  node->bytes = payload_len + label_len;
  node->event.kind = kind;
  node->event.peer = peer;
  node->event.channel = channel;
  node->event.channel_state = state;
  node->event.peer_state = peer_state;
  node->event.sdp_type = sdp_type;
  node->event.is_text = is_text;
  if (channel != NULL) {
    node->label = h2_web_webrtc_copy_string(channel->info.label.data,
                                            channel->info.label.len);
    if (node->label == NULL)
      goto fail;
    node->event.channel_info = channel->info;
    node->event.channel_info.label.data = node->label;
  }
  if (payload_len != 0u) {
    node->payload = malloc(payload_len + 1u);
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
  h2_web_webrtc_wake_js((uintptr_t)peer);
  return 1;
fail:
  free(node->label);
  free(node->payload);
  free(node);
  h2_web_webrtc_fail((uintptr_t)peer, H2_PAL_ERR_NO_MEMORY);
  return 0;
}

static h2_pal_result_t h2_web_webrtc_dequeue(h2_pal_webrtc_peer_t *peer,
                                             h2_pal_webrtc_event_t *out_event) {
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

static char *h2_web_webrtc_copy_string(const char *data, size_t len) {
  if ((data == NULL && len != 0u) || len == SIZE_MAX)
    return NULL;
  char *copy = malloc(len + 1u);
  if (copy == NULL)
    return NULL;
  if (len != 0u)
    memcpy(copy, data, len);
  copy[len] = '\0';
  return copy;
}

static h2_pal_webrtc_channel_t *
h2_web_webrtc_find_channel(h2_pal_webrtc_peer_t *peer, uintptr_t address) {
  for (h2_pal_webrtc_channel_t *channel = peer == NULL ? NULL : peer->channels;
       channel != NULL; channel = channel->next) {
    if ((uintptr_t)channel == address)
      return channel;
  }
  return NULL;
}

EM_JS(int, h2_web_webrtc_peer_create_js, (uintptr_t peer_address), {
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
      wakePoll : null,
      cancelOffer : null
    };
    peers.set(peer_address, entry);
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

EM_JS(int, h2_web_webrtc_add_ice_js,
      (uintptr_t peer_address, const char *url, size_t url_len,
       const char *username, size_t username_len, const char *credential,
       size_t credential_len),
      {
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

EM_ASYNC_JS(int, h2_web_webrtc_start_offer_js, (uintptr_t peer_address), {
  const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
  if (!entry)
    return -10;
  try {
    const pc = entry.pc;
    const offer = await pc.createOffer();
    if (Module['h2WebRtcPeers']?.get(peer_address) !== entry)
      return -10;
    await pc.setLocalDescription(offer);
    if (Module['h2WebRtcPeers']?.get(peer_address) !== entry)
      return -10;
    if (pc.iceGatheringState !== 'complete') {
      await new Promise((resolve) => {
        const finish = () => {
          pc.removeEventListener('icegatheringstatechange', changed);
          entry.cancelOffer = null;
          resolve();
        };
        const changed = () => {
          if (pc.iceGatheringState === 'complete')
            finish();
        };
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
});

EM_JS(int, h2_web_webrtc_set_media_track_js,
      (uintptr_t peer_address, uintptr_t token), {
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

EM_ASYNC_JS(int, h2_web_webrtc_unset_media_track_js, (uintptr_t peer_address), {
  const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
  if (!entry)
    return -10;
  const binding = entry.binding;
  if (!binding || binding.detaching)
    return -7;
  binding.detaching = true;
  try {
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
  }
});

EM_ASYNC_JS(int, h2_web_webrtc_set_remote_sdp_js,
            (uintptr_t peer_address, int type, const char *sdp, size_t sdp_len),
            {
              const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
              if (!entry)
                return -10;
              if (type !== 2)
                return -7;
              try {
                await entry.pc.setRemoteDescription(
                    {type : 'answer', sdp : UTF8ToString(sdp, sdp_len)});
                return Module['h2WebRtcPeers']
                    ?.get(peer_address) === entry ? 0 : -10;
              }
              catch(_) { return -4; }
            });

EM_JS(int, h2_web_webrtc_channel_create_js,
      (uintptr_t peer_address, uintptr_t channel_address, const char *label,
       size_t label_len, int has_stream_id, uint16_t stream_id, int ordered,
       int reliable),
      {
        const entry = Module['h2WebRtcPeers'] ?.get(peer_address);
        if (!entry)
          return -10;
        try {
          const options = {ordered : !!ordered};
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

EM_JS(int, h2_web_webrtc_channel_send_js,
      (uintptr_t channel_address, const uint8_t *data, size_t len, int is_text),
      {
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

EM_JS(void, h2_web_webrtc_channel_close_js, (uintptr_t channel_address), {
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

EM_JS(void, h2_web_webrtc_peer_close_js, (uintptr_t peer_address), {
  const peers = Module['h2WebRtcPeers'];
  const entry = peers ?.get(peer_address);
  if (!entry)
    return;
  peers.delete(peer_address);
  if (entry.wakePoll)
    entry.wakePoll(-10);
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
  if (entry.cancelOffer)
    entry.cancelOffer();
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

static void h2_web_webrtc_unlink_channel(h2_pal_webrtc_channel_t *channel) {
  h2_pal_webrtc_channel_t **cursor = &channel->peer->channels;
  while (*cursor != NULL && *cursor != channel)
    cursor = &(*cursor)->next;
  if (*cursor == channel)
    *cursor = channel->next;
}

static void h2_web_webrtc_free_channel(h2_pal_webrtc_channel_t *channel) {
  free(channel->label);
  free(channel);
}

static void h2_web_webrtc_peer_close_now(h2_pal_webrtc_peer_t *peer) {
  h2_web_platform_t *owner = peer->owner;
  h2_web_webrtc_peer_close_js((uintptr_t)peer);
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
  free(peer);
}

static bool h2_web_webrtc_peer_end_async(h2_pal_webrtc_peer_t *peer) {
  if (--peer->async_calls == 0u && peer->close_pending) {
    h2_web_webrtc_peer_close_now(peer);
    return true;
  }
  return false;
}

EMSCRIPTEN_KEEPALIVE void h2_web_webrtc_peer_state(uintptr_t peer_address,
                                                   int state) {
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
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  if (peer == NULL || peer->closed)
    return;
  (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_LOCAL_SDP, NULL, 0, 0,
                              (h2_pal_webrtc_sdp_type_t)type, sdp, sdp_len, 0);
}

static h2_pal_webrtc_channel_t *
h2_web_webrtc_new_channel(h2_pal_webrtc_peer_t *peer, const char *label,
                          size_t label_len, uint16_t stream_id,
                          int has_stream_id, int ordered, int reliable) {
  h2_pal_webrtc_channel_t *channel = calloc(1u, sizeof(*channel));
  if (channel == NULL)
    return NULL;
  channel->label = h2_web_webrtc_copy_string(label, label_len);
  if (channel->label == NULL) {
    free(channel);
    return NULL;
  }
  channel->peer = peer;
  channel->info = (h2_pal_webrtc_channel_info_t){
      .label = {.data = channel->label, .len = label_len},
      .stream_id = stream_id,
      .has_stream_id = has_stream_id != 0,
      .ordered = ordered != 0,
      .reliable = reliable != 0,
  };
  channel->next = peer->channels;
  peer->channels = channel;
  return channel;
}

EMSCRIPTEN_KEEPALIVE uintptr_t h2_web_webrtc_remote_channel(
    uintptr_t peer_address, const char *label, size_t label_len,
    uint16_t stream_id, int has_stream_id, int ordered, int reliable) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  if (peer == NULL || peer->closed || (label == NULL && label_len != 0u))
    return 0u;
  if (peer->event_error != H2_PAL_OK)
    return 0u;
  h2_pal_webrtc_channel_t *channel = h2_web_webrtc_new_channel(
      peer, label, label_len, stream_id, has_stream_id, ordered, reliable);
  if (channel == NULL)
    h2_web_webrtc_fail(peer_address, H2_PAL_ERR_NO_MEMORY);
  return (uintptr_t)channel;
}

EMSCRIPTEN_KEEPALIVE void
h2_web_webrtc_channel_metadata(uintptr_t peer_address,
                               uintptr_t channel_address, uint16_t stream_id,
                               int has_stream_id) {
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
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  h2_pal_webrtc_channel_t *channel =
      h2_web_webrtc_find_channel(peer, channel_address);
  if (channel == NULL || channel->terminal || peer->closed)
    return;
  (void)h2_web_webrtc_enqueue(peer, H2_PAL_WEBRTC_EVENT_WRITABLE, channel, 0, 0,
                              0, NULL, 0u, 0);
}

static h2_pal_result_t
h2_web_webrtc_peer_create(void *user, h2_pal_webrtc_peer_t **out_peer) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || out_peer == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  *out_peer = NULL;
  h2_pal_webrtc_peer_t *peer = calloc(1u, sizeof(*peer));
  if (peer == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  peer->owner = platform;
  peer->state = H2_PAL_WEBRTC_PEER_NEW;
  const h2_pal_result_t result =
      (h2_pal_result_t)h2_web_webrtc_peer_create_js((uintptr_t)peer);
  if (result != H2_PAL_OK) {
    free(peer);
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
  if (peer == NULL || server == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (peer->offer_started)
    return H2_PAL_ERR_INVALID_STATE;
  return (h2_pal_result_t)h2_web_webrtc_add_ice_js(
      (uintptr_t)peer, server->url.data, server->url.len, server->username.data,
      server->username.len, server->credential.data, server->credential.len);
}

static h2_pal_result_t
h2_web_webrtc_peer_start_offer(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (peer->offer_started)
    return H2_PAL_ERR_INVALID_STATE;
  peer->offer_started = true;
  peer->async_calls++;
  h2_pal_result_t result =
      (h2_pal_result_t)h2_web_webrtc_start_offer_js((uintptr_t)peer);
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
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  peer->async_calls++;
  h2_pal_result_t rc = (h2_pal_result_t)h2_web_webrtc_set_remote_sdp_js(
      (uintptr_t)peer, type, sdp.data, sdp.len);
  if (peer->closed)
    rc = H2_PAL_ERR_CLOSED;
  (void)h2_web_webrtc_peer_end_async(peer);
  return rc;
}

static h2_pal_result_t h2_web_webrtc_peer_create_data_channel(
    h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_channel_config_t *config,
    h2_pal_webrtc_channel_t **out_channel) {
  if (peer == NULL || config == NULL || out_channel == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  *out_channel = NULL;
  if (peer->event_error != H2_PAL_OK)
    return peer->event_error;
  h2_pal_webrtc_channel_t *channel = h2_web_webrtc_new_channel(
      peer, config->label.data, config->label.len, config->stream_id,
      config->has_stream_id, config->ordered, config->reliable);
  if (channel == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  const h2_pal_result_t result =
      (h2_pal_result_t)h2_web_webrtc_channel_create_js(
          (uintptr_t)peer, (uintptr_t)channel, config->label.data,
          config->label.len, config->has_stream_id, config->stream_id,
          config->ordered, config->reliable);
  if (result != H2_PAL_OK) {
    h2_web_webrtc_unlink_channel(channel);
    h2_web_webrtc_free_channel(channel);
    return result;
  }
  *out_channel = channel;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_webrtc_peer_set_track(h2_pal_webrtc_peer_t *peer,
                             h2_pal_webrtc_track_t *track) {
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (track == NULL || track->native_handle == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (peer->offer_started || peer->media_track != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_result_t rc = (h2_pal_result_t)h2_web_webrtc_set_media_track_js(
      (uintptr_t)peer, (uintptr_t)track->native_handle);
  if (rc == H2_PAL_OK)
    peer->media_track = track;
  return rc;
}

static h2_pal_result_t
h2_web_webrtc_peer_unset_track(h2_pal_webrtc_peer_t *peer,
                               h2_pal_webrtc_track_t *track) {
  if (peer == NULL || track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (peer->media_track != track || peer->track_detaching)
    return H2_PAL_ERR_INVALID_STATE;
  peer->track_detaching = true;
  peer->async_calls++;
  h2_pal_result_t rc =
      (h2_pal_result_t)h2_web_webrtc_unset_media_track_js((uintptr_t)peer);
  if (peer->closed)
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK)
    peer->media_track = NULL;
  peer->track_detaching = false;
  (void)h2_web_webrtc_peer_end_async(peer);
  return rc;
}

EM_ASYNC_JS(int, h2_web_webrtc_wait_js, (uintptr_t address, int timeout_ms), {
  const entry = Module['h2WebRtcPeers'] ?.get(address);
  if (!entry)
    return -10;
  return await new Promise(resolve => {
    const finish = result => {
      clearTimeout(timer);
      entry.wakePoll = null;
      resolve(result);
    };
    const timer = setTimeout(() => finish(-6), timeout_ms);
    entry.wakePoll = finish;
  });
});

static h2_pal_result_t h2_web_webrtc_peer_poll(h2_pal_webrtc_peer_t *peer,
                                               int timeout_ms,
                                               h2_pal_webrtc_event_t *event) {
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
  result = (h2_pal_result_t)h2_web_webrtc_wait_js((uintptr_t)peer, timeout_ms);
  if (peer->closed)
    result = H2_PAL_ERR_CLOSED;
  else if (result == H2_PAL_OK)
    result = h2_web_webrtc_dequeue(peer, event);
  peer->poll_waiting = false;
  (void)h2_web_webrtc_peer_end_async(peer);
  return result;
}

static h2_pal_result_t h2_web_webrtc_peer_send_opus(h2_pal_webrtc_peer_t *peer,
                                                    const uint8_t *opus,
                                                    size_t opus_len) {
  (void)opus;
  (void)opus_len;
  return peer == NULL || peer->closed ? H2_PAL_ERR_CLOSED
                                      : H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t
h2_web_webrtc_channel_send(h2_pal_webrtc_channel_t *channel,
                           const uint8_t *data, size_t len, int is_text) {
  if (channel == NULL || channel->terminal)
    return H2_PAL_ERR_CLOSED;
  if (channel->peer->event_error != H2_PAL_OK)
    return channel->peer->event_error;
  return (h2_pal_result_t)h2_web_webrtc_channel_send_js((uintptr_t)channel,
                                                        data, len, is_text);
}

static void h2_web_webrtc_channel_close(h2_pal_webrtc_channel_t *channel) {
  if (channel == NULL || channel->terminal)
    return;
  channel->terminal = true;
  h2_web_webrtc_channel_close_js((uintptr_t)channel);
  (void)h2_web_webrtc_enqueue(channel->peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE,
                              channel, H2_PAL_WEBRTC_CHANNEL_CLOSED, 0, 0, NULL,
                              0u, 0);
}

static void h2_web_webrtc_peer_close(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL || peer->closed)
    return;
  peer->closed = true;
  // Stop browser callbacks and cancel ICE waiting immediately; only the C
  // allocation waits for outstanding Asyncify frames to return.
  h2_web_webrtc_peer_close_js((uintptr_t)peer);
  peer->media_track = NULL;
  if (peer->async_calls != 0u) {
    peer->close_pending = true;
    return;
  }
  h2_web_webrtc_peer_close_now(peer);
}

static const h2_pal_webrtc_vtable_t h2_web_webrtc_vtable = {
    .peer_create = h2_web_webrtc_peer_create,
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
  platform->webrtc_api = (h2_pal_webrtc_api_t){
      .user = platform,
      .vtable = &h2_web_webrtc_vtable,
  };
}

void h2_web_platform_webrtc_deinit(h2_web_platform_t *platform) {
  while (platform->webrtc_peers != NULL) {
    h2_pal_webrtc_peer_t *peer = platform->webrtc_peers;
    peer->closed = true;
    h2_web_webrtc_peer_close_now(peer);
  }
}

bool h2_web_platform_webrtc_busy(h2_web_platform_t *platform) {
  for (h2_pal_webrtc_peer_t *peer = platform->webrtc_peers; peer != NULL;
       peer = peer->next)
    if (peer->async_calls != 0u)
      return true;
  return false;
}
