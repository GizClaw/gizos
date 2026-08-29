#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_webrtc_channel {
  h2_pal_webrtc_peer_t *peer;
  h2_pal_webrtc_channel_t *next;
  h2_pal_webrtc_channel_info_t info;
  char *label;
  unsigned dispatch_depth;
  bool terminal;
  bool close_pending;
};

struct h2_pal_webrtc_peer {
  h2_web_platform_t *owner;
  h2_pal_webrtc_peer_t *next;
  h2_pal_webrtc_callbacks_t callbacks;
  h2_pal_webrtc_channel_t *channels;
  h2_pal_webrtc_peer_state_t state;
  unsigned dispatch_depth;
  bool closed;
  bool close_pending;
};

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
      mediaEnabled : false,
      localStream : null,
      remoteAudio : null
    };
    peers.set(peer_address, entry);
    const bindChannel = (channelAddress, dc) => {
      const channelEntry = {peerAddress : peer_address, dc};
      channels.set(channelAddress, channelEntry);
      dc.binaryType = 'arraybuffer';
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
      const terminal = (state) => {
        if (!live())
          return;
        channels.delete(channelAddress);
        Module['_h2_web_webrtc_channel_state'](peer_address, channelAddress,
                                               state);
        if (state === 3) {
          dc.onopen = dc.onclose = dc.onerror = dc.onmessage = null;
          try { dc.close(); } catch (_) {}
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
        const buffer = bytes.byteLength ? _malloc(bytes.byteLength) : 0;
        if (bytes.byteLength && !buffer) {
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
      if (peers.get(peer_address) !== entry || !entry.mediaEnabled ||
          event.track.kind !== 'audio')
        return;
      const stream = event.streams && event.streams[0]
          ? event.streams[0]
          : new MediaStream([event.track]);
      const audio = entry.remoteAudio || new Audio();
      entry.remoteAudio = audio;
      audio.autoplay = true;
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
    if (entry.mediaEnabled && !entry.localStream) {
      const addReceiveAudio = (reason) => {
        pc.addTransceiver('audio', {direction : 'recvonly'});
        console.warn(`H2 WebRTC: ${reason}; using receive-only audio`);
      };
      if (globalThis.navigator?.mediaDevices?.getUserMedia) {
        try {
          const stream = await navigator.mediaDevices.getUserMedia({audio : true});
          if (Module['h2WebRtcPeers']?.get(peer_address) !== entry) {
            for (const track of stream.getTracks()) track.stop();
            return -10;
          }
          entry.localStream = stream;
          const audioTracks = stream.getAudioTracks();
          if (!audioTracks.length) {
            for (const track of stream.getTracks()) track.stop();
            entry.localStream = null;
            addReceiveAudio('microphone returned no audio track');
          } else {
            for (const track of audioTracks) pc.addTrack(track, stream);
          }
        } catch(error) {
          if (Module['h2WebRtcPeers']?.get(peer_address) !== entry)
            return -10;
          addReceiveAudio(`microphone unavailable (${error?.name || 'error'})`);
        }
      } else {
        addReceiveAudio('getUserMedia unavailable');
      }
    }
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    if (pc.iceGatheringState !== 'complete') {
      await new Promise((resolve) => {
        const changed = () => {
          if (pc.iceGatheringState === 'complete') {
            pc.removeEventListener('icegatheringstatechange', changed);
            resolve();
          }
        };
        pc.addEventListener('icegatheringstatechange', changed);
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
  catch(error) {
    if (entry.localStream) {
      for (const track of entry.localStream.getTracks()) track.stop();
      entry.localStream = null;
    }
    console.error('H2 WebRTC: failed to create offer', error);
    return -4;
  }
});

EM_JS(int, h2_web_webrtc_set_media_track_js,
      (uintptr_t peer_address, int enabled), {
        const entry = Module['h2WebRtcPeers']?.get(peer_address);
        if (!entry)
          return -10;
        if (entry.pc.signalingState !== 'stable' || entry.pc.localDescription)
          return -7;
        entry.mediaEnabled = !!enabled;
        return 0;
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
  try { entry.dc.close(); }
  catch(_) {}
});

EM_JS(void, h2_web_webrtc_peer_close_js, (uintptr_t peer_address), {
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
      try { channel.dc.close(); }
      catch(_) {}
    }
  }
  entry.pc.onconnectionstatechange = entry.pc.ondatachannel =
      entry.pc.ontrack = null;
  if (entry.localStream) {
    for (const track of entry.localStream.getTracks()) track.stop();
    entry.localStream = null;
  }
  if (entry.remoteAudio) {
    entry.remoteAudio.pause();
    entry.remoteAudio.srcObject = null;
    entry.remoteAudio = null;
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

static void h2_web_webrtc_channel_close_now(h2_pal_webrtc_channel_t *channel) {
  h2_web_webrtc_channel_close_js((uintptr_t)channel);
  h2_web_webrtc_unlink_channel(channel);
  h2_web_webrtc_free_channel(channel);
}

static void h2_web_webrtc_peer_close_now(h2_pal_webrtc_peer_t *peer) {
  h2_web_platform_t *owner = peer->owner;
  h2_web_webrtc_peer_close_js((uintptr_t)peer);
  if (owner->webrtc_audio_track.bound_peer == peer)
    owner->webrtc_audio_track.bound_peer = NULL;
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

static bool h2_web_webrtc_peer_end_dispatch(h2_pal_webrtc_peer_t *peer) {
  if (--peer->dispatch_depth == 0u && peer->close_pending) {
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
  if (peer->callbacks.on_peer_state != NULL) {
    peer->dispatch_depth++;
    peer->callbacks.on_peer_state(peer->callbacks.user, peer, peer->state);
    (void)h2_web_webrtc_peer_end_dispatch(peer);
  }
}

EMSCRIPTEN_KEEPALIVE void h2_web_webrtc_local_sdp(uintptr_t peer_address,
                                                  int type, const char *sdp,
                                                  size_t sdp_len) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  if (peer == NULL || peer->closed || peer->callbacks.on_local_sdp == NULL)
    return;
  const h2_pal_webrtc_str_t view = {.data = sdp, .len = sdp_len};
  peer->dispatch_depth++;
  peer->callbacks.on_local_sdp(peer->callbacks.user, peer,
                               (h2_pal_webrtc_sdp_type_t)type, view);
  (void)h2_web_webrtc_peer_end_dispatch(peer);
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
  return (uintptr_t)h2_web_webrtc_new_channel(peer, label, label_len, stream_id,
                                              has_stream_id, ordered, reliable);
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
  bool terminal = state != H2_PAL_WEBRTC_CHANNEL_OPEN;
  channel->terminal = terminal;
  if (peer->callbacks.on_channel_state != NULL) {
    peer->dispatch_depth++;
    channel->dispatch_depth++;
    peer->callbacks.on_channel_state(peer->callbacks.user, peer, channel,
                                     &channel->info,
                                     (h2_pal_webrtc_channel_state_t)state);
    channel->dispatch_depth--;
    if (channel->close_pending && channel->dispatch_depth == 0u)
      terminal = true;
    if (h2_web_webrtc_peer_end_dispatch(peer))
      return;
  }
  if (terminal && channel->dispatch_depth == 0u) {
    if (channel->close_pending) {
      h2_web_webrtc_channel_close_now(channel);
    } else {
      h2_web_webrtc_unlink_channel(channel);
      h2_web_webrtc_free_channel(channel);
    }
  }
}

EMSCRIPTEN_KEEPALIVE void
h2_web_webrtc_channel_message(uintptr_t peer_address, uintptr_t channel_address,
                              const uint8_t *data, size_t len, int is_text) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_address;
  h2_pal_webrtc_channel_t *channel =
      h2_web_webrtc_find_channel(peer, channel_address);
  if (channel == NULL || channel->terminal || peer->closed ||
      peer->callbacks.on_channel_message == NULL)
    return;
  peer->dispatch_depth++;
  channel->dispatch_depth++;
  peer->callbacks.on_channel_message(peer->callbacks.user, peer, channel,
                                     &channel->info, data, len, is_text);
  channel->dispatch_depth--;
  const bool close_channel = channel->close_pending;
  if (h2_web_webrtc_peer_end_dispatch(peer))
    return;
  if (close_channel)
    h2_web_webrtc_channel_close_now(channel);
}

static h2_pal_result_t
h2_web_webrtc_peer_create(void *user,
                          const h2_pal_webrtc_callbacks_t *callbacks,
                          h2_pal_webrtc_peer_t **out_peer) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || callbacks == NULL || out_peer == NULL ||
      platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  *out_peer = NULL;
  h2_pal_webrtc_peer_t *peer = calloc(1u, sizeof(*peer));
  if (peer == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  peer->owner = platform;
  peer->callbacks = *callbacks;
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
  return (h2_pal_result_t)h2_web_webrtc_add_ice_js(
      (uintptr_t)peer, server->url.data, server->url.len, server->username.data,
      server->username.len, server->credential.data, server->credential.len);
}

static h2_pal_result_t
h2_web_webrtc_peer_start_offer(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  peer->dispatch_depth++;
  h2_pal_result_t result =
      (h2_pal_result_t)h2_web_webrtc_start_offer_js((uintptr_t)peer);
  if (peer->closed && result == H2_PAL_OK)
    result = H2_PAL_ERR_CLOSED;
  (void)h2_web_webrtc_peer_end_dispatch(peer);
  return result;
}

static h2_pal_result_t
h2_web_webrtc_peer_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                                  h2_pal_webrtc_sdp_type_t type,
                                  h2_pal_webrtc_str_t sdp) {
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  return (h2_pal_result_t)h2_web_webrtc_set_remote_sdp_js((uintptr_t)peer, type,
                                                          sdp.data, sdp.len);
}

static h2_pal_result_t h2_web_webrtc_peer_create_data_channel(
    h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_channel_config_t *config,
    h2_pal_webrtc_channel_t **out_channel) {
  if (peer == NULL || config == NULL || out_channel == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  *out_channel = NULL;
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
h2_web_webrtc_peer_set_media_track(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_track_t *track) {
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (track != NULL &&
      (track->owner != peer->owner ||
       (track->bound_peer != NULL && track->bound_peer != peer))) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_webrtc_track_t *platform_track = &peer->owner->webrtc_audio_track;
  const h2_pal_result_t result =
      (h2_pal_result_t)h2_web_webrtc_set_media_track_js((uintptr_t)peer,
                                                        track != NULL);
  if (result == H2_PAL_OK) {
    if (platform_track->bound_peer == peer)
      platform_track->bound_peer = NULL;
    if (track != NULL)
      track->bound_peer = peer;
  }
  return result;
}

static h2_pal_result_t h2_web_webrtc_peer_poll(h2_pal_webrtc_peer_t *peer,
                                               int timeout_ms) {
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (timeout_ms < 0)
    return H2_PAL_ERR_INVALID_ARG;
  peer->dispatch_depth++;
  if (timeout_ms != 0)
    emscripten_sleep((unsigned)timeout_ms);
  const h2_pal_result_t result =
      peer->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
  (void)h2_web_webrtc_peer_end_dispatch(peer);
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
  return (h2_pal_result_t)h2_web_webrtc_channel_send_js((uintptr_t)channel,
                                                        data, len, is_text);
}

static void h2_web_webrtc_channel_close(h2_pal_webrtc_channel_t *channel) {
  if (channel == NULL || channel->terminal)
    return;
  channel->terminal = true;
  if (channel->dispatch_depth != 0u) {
    channel->close_pending = true;
    return;
  }
  h2_web_webrtc_channel_close_now(channel);
}

static void h2_web_webrtc_peer_close(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL || peer->closed)
    return;
  peer->closed = true;
  if (peer->dispatch_depth != 0u) {
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
    .peer_set_media_track = h2_web_webrtc_peer_set_media_track,
    .peer_poll = h2_web_webrtc_peer_poll,
    .peer_send_opus = h2_web_webrtc_peer_send_opus,
    .channel_send = h2_web_webrtc_channel_send,
    .channel_close = h2_web_webrtc_channel_close,
    .peer_close = h2_web_webrtc_peer_close,
};

void h2_web_platform_webrtc_init(h2_web_platform_t *platform) {
  platform->webrtc_audio_track.owner = platform;
  platform->webrtc_audio_track.bound_peer = NULL;
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
  platform->webrtc_audio_track.owner = NULL;
  platform->webrtc_audio_track.bound_peer = NULL;
}
