#include "h2_pion.h"

#include "h2_pion_bridge.h"
#include "libs/pal/providers/pion/pion_archive.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_webrtc_channel {
  struct h2_pal_webrtc_peer *peer;
  struct h2_pion *provider;
  struct h2_pal_webrtc_channel *next;
  uint64_t key;
  char *label;
  h2_pal_webrtc_channel_info_t info;
  int terminal;
};

struct h2_pal_webrtc_peer {
  struct h2_pion *owner;
  struct h2_pal_webrtc_peer *next;
  h2_pal_webrtc_callbacks_t callbacks;
  struct h2_pal_webrtc_channel *channels;
  uint64_t go_handle;
  uint64_t next_channel_key;
  h2_pal_webrtc_peer_state_t state;
  unsigned operation_depth;
  int closed;
  int close_pending;
};

struct h2_pion {
  h2_pal_mem_api_t mem;
  h2_pal_webrtc_api_t api;
  struct h2_pal_webrtc_peer *peers;
  unsigned operation_depth;
  int destroy_pending;
};

static void h2_pion_peer_close_now(h2_pal_webrtc_peer_t *peer);
static void h2_pion_finish_pending_destroy(h2_pion_t *provider);

static void *h2_pion_alloc(h2_pion_t *provider, size_t size) {
  void *ptr = h2_pal_mem_alloc(&provider->mem, size);
  if (ptr != NULL) {
    memset(ptr, 0, size);
  }
  return ptr;
}

static char *h2_pion_copy_string(h2_pion_t *provider, const char *data,
                                 size_t len) {
  if ((data == NULL && len != 0u) || len == SIZE_MAX) {
    return NULL;
  }
  char *copy = h2_pion_alloc(provider, len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  if (len != 0u) {
    memcpy(copy, data, len);
  }
  return copy;
}

static h2_pal_webrtc_channel_t *h2_pion_find_channel(h2_pal_webrtc_peer_t *peer,
                                                     uint64_t key) {
  for (h2_pal_webrtc_channel_t *channel = peer->channels; channel != NULL;
       channel = channel->next) {
    if (channel->key == key) {
      return channel;
    }
  }
  return NULL;
}

static void h2_pion_unlink_channel(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_channel_t *channel) {
  h2_pal_webrtc_channel_t **cursor = &peer->channels;
  while (*cursor != NULL && *cursor != channel) {
    cursor = &(*cursor)->next;
  }
  if (*cursor == channel) {
    *cursor = channel->next;
  }
}

static void h2_pion_free_channel(h2_pal_webrtc_channel_t *channel) {
  h2_pion_t *provider = channel->provider;
  h2_pal_mem_free(&provider->mem, channel->label);
  h2_pal_mem_free(&provider->mem, channel);
}

static h2_pal_webrtc_channel_t *
h2_pion_new_channel(h2_pal_webrtc_peer_t *peer, uint64_t key, const char *label,
                    size_t label_len, int has_stream_id, uint16_t stream_id,
                    int ordered, int reliable) {
  h2_pal_webrtc_channel_t *channel =
      h2_pion_alloc(peer->owner, sizeof(*channel));
  if (channel == NULL) {
    return NULL;
  }
  channel->label = h2_pion_copy_string(peer->owner, label, label_len);
  if (channel->label == NULL) {
    h2_pal_mem_free(&peer->owner->mem, channel);
    return NULL;
  }
  channel->peer = peer;
  channel->provider = peer->owner;
  channel->key = key;
  channel->info.label.data = channel->label;
  channel->info.label.len = label_len;
  channel->info.has_stream_id = has_stream_id != 0;
  channel->info.stream_id = stream_id;
  channel->info.ordered = ordered != 0;
  channel->info.reliable = reliable != 0;
  channel->next = peer->channels;
  peer->channels = channel;
  return channel;
}

static void h2_pion_emit_peer_state(h2_pal_webrtc_peer_t *peer,
                                    h2_pal_webrtc_peer_state_t state) {
  if (peer->state == state) {
    return;
  }
  peer->state = state;
  if (peer->callbacks.on_peer_state != NULL) {
    peer->callbacks.on_peer_state(peer->callbacks.user, peer, state);
  }
}

static void h2_pion_begin_operation(h2_pal_webrtc_peer_t *peer) {
  peer->operation_depth++;
  peer->owner->operation_depth++;
}

static void h2_pion_finish_destroy(h2_pion_t *provider) {
  if (!provider->destroy_pending || provider->operation_depth != 0u ||
      provider->peers != NULL) {
    return;
  }
  h2_pal_mem_api_t mem = provider->mem;
  h2_pal_mem_free(&mem, provider);
}

static h2_pal_result_t h2_pion_end_operation(h2_pal_webrtc_peer_t *peer,
                                             h2_pal_result_t result) {
  h2_pion_t *provider = peer->owner;
  peer->operation_depth--;
  if (peer->operation_depth == 0u && peer->close_pending) {
    h2_pion_peer_close_now(peer);
  }
  provider->operation_depth--;
  h2_pion_finish_pending_destroy(provider);
  return result;
}

static h2_pal_result_t
h2_pion_peer_create(void *user, const h2_pal_webrtc_callbacks_t *callbacks,
                    h2_pal_webrtc_peer_t **out_peer) {
  h2_pion_t *provider = user;
  if (provider == NULL || callbacks == NULL || out_peer == NULL ||
      provider->destroy_pending) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_peer = NULL;
  h2_pal_webrtc_peer_t *peer = h2_pion_alloc(provider, sizeof(*peer));
  if (peer == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  peer->owner = provider;
  peer->callbacks = *callbacks;
  peer->next_channel_key = 1u;
  peer->state = H2_PAL_WEBRTC_PEER_NEW;
  peer->go_handle = h2PionGoPeerCreate();
  if (peer->go_handle == 0u) {
    h2_pal_mem_free(&provider->mem, peer);
    return H2_PAL_ERR_IO;
  }
  peer->next = provider->peers;
  provider->peers = peer;
  *out_peer = peer;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_pion_peer_add_ice_server(h2_pal_webrtc_peer_t *peer,
                            const h2_pal_webrtc_ice_server_t *server) {
  if (peer == NULL || server == NULL || peer->closed) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return (h2_pal_result_t)h2PionGoPeerAddICEServer(
      peer->go_handle, (char *)server->url.data, server->url.len,
      (char *)server->username.data, server->username.len,
      (char *)server->credential.data, server->credential.len);
}

static h2_pal_result_t h2_pion_peer_start_offer(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL || peer->closed) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  char *sdp = NULL;
  size_t sdp_len = 0u;
  h2_pal_result_t result =
      (h2_pal_result_t)h2PionGoPeerStartOffer(peer->go_handle, &sdp, &sdp_len);
  if (result == H2_PAL_OK && peer->callbacks.on_local_sdp != NULL) {
    h2_pal_webrtc_str_t view = {.data = sdp, .len = sdp_len};
    h2_pion_begin_operation(peer);
    peer->callbacks.on_local_sdp(peer->callbacks.user, peer,
                                 H2_PAL_WEBRTC_SDP_OFFER, view);
    if ((peer->closed || peer->owner->destroy_pending) &&
        result == H2_PAL_OK) {
      result = H2_PAL_ERR_CLOSED;
    }
    result = h2_pion_end_operation(peer, result);
  }
  free(sdp);
  return result;
}

static h2_pal_result_t
h2_pion_peer_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                            h2_pal_webrtc_sdp_type_t type,
                            h2_pal_webrtc_str_t sdp) {
  if (peer == NULL || peer->closed || type != H2_PAL_WEBRTC_SDP_ANSWER) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return (h2_pal_result_t)h2PionGoPeerSetRemoteSDP(peer->go_handle,
                                                   (char *)sdp.data, sdp.len);
}

static h2_pal_result_t
h2_pion_peer_create_data_channel(h2_pal_webrtc_peer_t *peer,
                                 const h2_pal_webrtc_channel_config_t *config,
                                 h2_pal_webrtc_channel_t **out_channel) {
  if (peer == NULL || config == NULL || out_channel == NULL || peer->closed) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *out_channel = NULL;
  uint64_t key = peer->next_channel_key++;
  h2_pal_webrtc_channel_t *channel = h2_pion_new_channel(
      peer, key, config->label.data, config->label.len, config->has_stream_id,
      config->stream_id, config->ordered, config->reliable);
  if (channel == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  h2_pal_result_t result = (h2_pal_result_t)h2PionGoPeerCreateDataChannel(
      peer->go_handle, key, (char *)config->label.data, config->label.len,
      config->has_stream_id, config->stream_id, config->ordered,
      config->reliable);
  if (result != H2_PAL_OK) {
    h2_pion_unlink_channel(peer, channel);
    h2_pion_free_channel(channel);
    return result;
  }
  *out_channel = channel;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_pion_peer_poll(h2_pal_webrtc_peer_t *peer,
                                         int timeout_ms) {
  if (peer == NULL || peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  if (timeout_ms < 0) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pion_begin_operation(peer);
  h2_pal_result_t result = (h2_pal_result_t)h2PionGoPeerPoll(
      peer->go_handle, (uintptr_t)peer, timeout_ms);
  if ((peer->closed || peer->owner->destroy_pending) &&
      result == H2_PAL_OK) {
    result = H2_PAL_ERR_CLOSED;
  }
  return h2_pion_end_operation(peer, result);
}

static h2_pal_result_t h2_pion_peer_send_opus(h2_pal_webrtc_peer_t *peer,
                                              const uint8_t *opus,
                                              size_t opus_len) {
  if (peer == NULL || peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  return (h2_pal_result_t)h2PionGoPeerSendOpus(peer->go_handle, (uint8_t *)opus,
                                               opus_len);
}

static h2_pal_result_t h2_pion_channel_send(h2_pal_webrtc_channel_t *channel,
                                            const uint8_t *data, size_t len,
                                            int is_text) {
  if (channel == NULL || channel->terminal || channel->peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  return (h2_pal_result_t)h2PionGoChannelSend(
      channel->peer->go_handle, channel->key, (uint8_t *)data, len, is_text);
}

static void h2_pion_channel_close(h2_pal_webrtc_channel_t *channel) {
  if (channel == NULL || channel->terminal) {
    return;
  }
  h2_pal_webrtc_peer_t *peer = channel->peer;
  channel->terminal = 1;
  h2PionGoChannelClose(peer->go_handle, channel->key);
  h2_pion_unlink_channel(peer, channel);
  if (peer->callbacks.on_channel_state != NULL && !peer->closed) {
    h2_pion_begin_operation(peer);
    peer->callbacks.on_channel_state(peer->callbacks.user, peer, channel,
                                     &channel->info,
                                     H2_PAL_WEBRTC_CHANNEL_CLOSED);
    h2_pion_free_channel(channel);
    (void)h2_pion_end_operation(peer, H2_PAL_OK);
    return;
  }
  h2_pion_free_channel(channel);
}

static void h2_pion_peer_close_now(h2_pal_webrtc_peer_t *peer) {
  h2_pion_t *provider = peer->owner;
  peer->closed = 1;
  peer->close_pending = 0;
  h2PionGoPeerDestroy(peer->go_handle);
  peer->go_handle = 0u;
  while (peer->channels != NULL) {
    h2_pal_webrtc_channel_t *channel = peer->channels;
    peer->channels = channel->next;
    channel->terminal = 1;
    if (peer->callbacks.on_channel_state != NULL) {
      peer->callbacks.on_channel_state(peer->callbacks.user, peer, channel,
                                       &channel->info,
                                       H2_PAL_WEBRTC_CHANNEL_CLOSED);
    }
    h2_pion_free_channel(channel);
  }
  h2_pion_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CLOSED);
  h2_pal_webrtc_peer_t **cursor = &provider->peers;
  while (*cursor != NULL && *cursor != peer) {
    cursor = &(*cursor)->next;
  }
  if (*cursor == peer) {
    *cursor = peer->next;
  }
  h2_pal_mem_free(&provider->mem, peer);
}

static void h2_pion_peer_close(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL || peer->close_pending) {
    return;
  }
  if (peer->operation_depth != 0u) {
    peer->closed = 1;
    peer->close_pending = 1;
    return;
  }
  if (!peer->closed) {
    h2_pion_t *provider = peer->owner;
    provider->operation_depth++;
    h2_pion_peer_close_now(peer);
    provider->operation_depth--;
    h2_pion_finish_pending_destroy(provider);
  }
}

static const h2_pal_webrtc_vtable_t s_h2_pion_vtable = {
    .peer_create = h2_pion_peer_create,
    .peer_add_ice_server = h2_pion_peer_add_ice_server,
    .peer_start_offer = h2_pion_peer_start_offer,
    .peer_set_remote_sdp = h2_pion_peer_set_remote_sdp,
    .peer_create_data_channel = h2_pion_peer_create_data_channel,
    .peer_poll = h2_pion_peer_poll,
    .peer_send_opus = h2_pion_peer_send_opus,
    .channel_send = h2_pion_channel_send,
    .channel_close = h2_pion_channel_close,
    .peer_close = h2_pion_peer_close,
};

h2_pal_result_t h2_pion_create(const h2_pion_config_t *config,
                                   h2_pion_t **out_provider) {
  if (config == NULL || out_provider == NULL || config->mem == NULL ||
      config->mem->vtable == NULL || config->mem->vtable->alloc == NULL ||
      config->mem->vtable->free == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_provider = NULL;
  h2_pion_t temporary = {.mem = *config->mem};
  h2_pion_t *provider = h2_pion_alloc(&temporary, sizeof(*provider));
  if (provider == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  provider->mem = *config->mem;
  provider->api.user = provider;
  provider->api.vtable = &s_h2_pion_vtable;
  *out_provider = provider;
  return H2_PAL_OK;
}

const h2_pal_webrtc_api_t *h2_pion_webrtc_api(h2_pion_t *provider) {
  return provider == NULL || provider->destroy_pending ? NULL : &provider->api;
}

void h2_pion_destroy(h2_pion_t **provider_ptr) {
  if (provider_ptr == NULL || *provider_ptr == NULL) {
    return;
  }
  h2_pion_t *provider = *provider_ptr;
  *provider_ptr = NULL;
  provider->destroy_pending = 1;
  h2_pion_finish_pending_destroy(provider);
}

static void h2_pion_finish_pending_destroy(h2_pion_t *provider) {
  if (provider == NULL || !provider->destroy_pending ||
      provider->operation_depth != 0u) {
    return;
  }
  provider->operation_depth++;
  while (provider->peers != NULL) {
    h2_pion_peer_close_now(provider->peers);
  }
  provider->operation_depth--;
  h2_pion_finish_destroy(provider);
}

void h2_pion_bridge_emit_peer_state(uintptr_t peer_key, int state) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed || peer->owner->destroy_pending ||
      state < H2_PAL_WEBRTC_PEER_NEW || state > H2_PAL_WEBRTC_PEER_CLOSED) {
    return;
  }
  h2_pion_emit_peer_state(peer, (h2_pal_webrtc_peer_state_t)state);
}

int h2_pion_bridge_emit_channel_open(uintptr_t peer_key, uint64_t channel_key,
                                     const char *label, size_t label_len,
                                     int has_stream_id, uint16_t stream_id,
                                     int ordered, int reliable, int remote) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed || peer->close_pending ||
      peer->owner->destroy_pending) {
    return H2_PAL_OK;
  }
  h2_pal_webrtc_channel_t *channel = h2_pion_find_channel(peer, channel_key);
  if (channel == NULL && remote) {
    channel = h2_pion_new_channel(peer, channel_key, label, label_len,
                                  has_stream_id, stream_id, ordered, reliable);
    if (channel == NULL) {
      return H2_PAL_ERR_NO_MEMORY;
    }
  }
  if (channel != NULL && !channel->terminal &&
      peer->callbacks.on_channel_state != NULL) {
    peer->callbacks.on_channel_state(peer->callbacks.user, peer, channel,
                                     &channel->info,
                                     H2_PAL_WEBRTC_CHANNEL_OPEN);
  }
  return peer->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
}

void h2_pion_bridge_emit_channel_state(uintptr_t peer_key,
                                       uint64_t channel_key, int state) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed || peer->close_pending ||
      peer->owner->destroy_pending ||
      (state != H2_PAL_WEBRTC_CHANNEL_CLOSED &&
       state != H2_PAL_WEBRTC_CHANNEL_ERROR)) {
    return;
  }
  h2_pal_webrtc_channel_t *channel = h2_pion_find_channel(peer, channel_key);
  if (channel == NULL || channel->terminal) {
    return;
  }
  channel->terminal = 1;
  h2_pion_unlink_channel(peer, channel);
  if (peer->callbacks.on_channel_state != NULL) {
    peer->callbacks.on_channel_state(peer->callbacks.user, peer, channel,
                                     &channel->info,
                                     (h2_pal_webrtc_channel_state_t)state);
  }
  h2_pion_free_channel(channel);
}

void h2_pion_bridge_emit_channel_message(uintptr_t peer_key,
                                         uint64_t channel_key,
                                         const uint8_t *data, size_t len,
                                         int is_text) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed || peer->close_pending ||
      peer->owner->destroy_pending ||
      peer->callbacks.on_channel_message == NULL) {
    return;
  }
  h2_pal_webrtc_channel_t *channel = h2_pion_find_channel(peer, channel_key);
  if (channel != NULL && !channel->terminal) {
    peer->callbacks.on_channel_message(peer->callbacks.user, peer, channel,
                                       &channel->info, data, len, is_text);
  }
}

void h2_pion_bridge_emit_opus_frame(uintptr_t peer_key, const uint8_t *data,
                                    size_t len) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer != NULL && !peer->closed && !peer->close_pending &&
      !peer->owner->destroy_pending && peer->callbacks.on_opus_frame != NULL) {
    peer->callbacks.on_opus_frame(peer->callbacks.user, peer, data, len);
  }
}
