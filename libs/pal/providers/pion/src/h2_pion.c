#include "h2_pion.h"

#include "h2_pion_bridge.h"
#include "libs/pal/providers/pion/pion_archive.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef H2_PION_TESTING
#define H2_PION_TESTING 0
#endif

struct h2_pal_webrtc_channel {
  struct h2_pal_webrtc_peer *peer;
  struct h2_pion *provider;
  struct h2_pal_webrtc_channel *next;
  uint64_t key;
  char *label;
  h2_pal_webrtc_channel_info_t info;
  int terminal;
};

typedef struct h2_pion_event {
  struct h2_pion_event *next;
  h2_pal_mem_api_t mem;
  h2_pal_webrtc_event_t event;
  char *label;
  uint8_t *payload;
} h2_pion_event_t;

struct h2_pal_webrtc_peer {
  struct h2_pion *owner;
  struct h2_pal_webrtc_peer *next;
  h2_pal_webrtc_track_t *media_track;
  h2_pion_event_t *event_head;
  h2_pion_event_t *event_tail;
  struct h2_pal_webrtc_channel *channels;
  uint64_t go_handle;
  uint64_t next_channel_key;
  h2_pal_webrtc_peer_state_t state;
  h2_pal_result_t media_result;
  uint8_t pending_opus[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
  size_t pending_opus_len;
  unsigned operation_depth;
  int offer_started;
  int closed;
  int close_pending;
#if H2_PION_TESTING
  size_t test_opus_send_attempts;
  size_t test_first_opus_len;
  uint64_t test_first_opus_hash;
  int test_opus_payload_mismatch;
  int test_block_next_opus_send;
#endif
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

#if H2_PION_TESTING
static uint64_t h2_pion_test_opus_hash(const uint8_t *data, size_t len) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t i = 0u; i < len; ++i) {
    hash ^= data[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}
#endif

static h2_pal_result_t h2_pion_submit_opus(h2_pal_webrtc_peer_t *peer,
                                           const uint8_t *opus,
                                           size_t opus_len) {
#if H2_PION_TESTING
  const uint64_t hash = h2_pion_test_opus_hash(opus, opus_len);
  if (peer->test_opus_send_attempts == 0u) {
    peer->test_first_opus_len = opus_len;
    peer->test_first_opus_hash = hash;
  } else if (peer->test_first_opus_len != opus_len ||
             peer->test_first_opus_hash != hash) {
    peer->test_opus_payload_mismatch = 1;
  }
  peer->test_opus_send_attempts++;
  if (peer->test_block_next_opus_send) {
    peer->test_block_next_opus_send = 0;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
#endif
  return (h2_pal_result_t)h2PionGoPeerSendOpus(peer->go_handle, (uint8_t *)opus,
                                               opus_len);
}

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

static void h2_pion_event_release(h2_pal_webrtc_event_t *event) {
  if (event == NULL || event->_private == NULL)
    return;
  h2_pion_event_t *node = event->_private;
  h2_pal_mem_free(&node->mem, node->label);
  h2_pal_mem_free(&node->mem, node->payload);
  h2_pal_mem_free(&node->mem, node);
  memset(event, 0, sizeof(*event));
}

static h2_pal_result_t h2_pion_enqueue_event(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_event_kind_t kind,
    h2_pal_webrtc_channel_t *channel, h2_pal_webrtc_channel_state_t state,
    h2_pal_webrtc_peer_state_t peer_state,
    h2_pal_webrtc_sdp_type_t sdp_type, const void *payload,
    size_t payload_len, int is_text) {
  if (peer == NULL || (payload == NULL && payload_len != 0u) ||
      payload_len == SIZE_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pion_event_t *node = h2_pion_alloc(peer->owner, sizeof(*node));
  if (node == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  node->mem = peer->owner->mem;
  node->event.kind = kind;
  node->event.peer = peer;
  node->event.channel = channel;
  node->event.channel_state = state;
  node->event.peer_state = peer_state;
  node->event.sdp_type = sdp_type;
  node->event.is_text = is_text;
  if (channel != NULL) {
    node->label = h2_pion_copy_string(peer->owner, channel->info.label.data,
                                      channel->info.label.len);
    if (node->label == NULL)
      goto no_memory;
    node->event.channel_info = channel->info;
    node->event.channel_info.label.data = node->label;
  }
  if (payload_len != 0u) {
    node->payload = h2_pion_alloc(peer->owner, payload_len + 1u);
    if (node->payload == NULL)
      goto no_memory;
    memcpy(node->payload, payload, payload_len);
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
  return H2_PAL_OK;

no_memory:
  h2_pal_mem_free(&peer->owner->mem, node->label);
  h2_pal_mem_free(&peer->owner->mem, node->payload);
  h2_pal_mem_free(&peer->owner->mem, node);
  return H2_PAL_ERR_NO_MEMORY;
}

static h2_pal_result_t h2_pion_dequeue_event(h2_pal_webrtc_peer_t *peer,
                                             h2_pal_webrtc_event_t *out) {
  h2_pion_event_t *node = peer->event_head;
  if (node == NULL)
    return H2_PAL_ERR_WOULD_BLOCK;
  peer->event_head = node->next;
  if (peer->event_head == NULL)
    peer->event_tail = NULL;
  *out = node->event;
  out->_private = node;
  out->_release = h2_pion_event_release;
  return H2_PAL_OK;
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
  (void)h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_PEER_STATE, NULL, 0,
                              state, 0, NULL, 0u, 0);
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

static h2_pal_result_t h2_pion_peer_create(void *user,
                    h2_pal_webrtc_peer_t **out_peer) {
  h2_pion_t *provider = user;
  if (provider == NULL || out_peer == NULL || provider->destroy_pending) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_peer = NULL;
  h2_pal_webrtc_peer_t *peer = h2_pion_alloc(provider, sizeof(*peer));
  if (peer == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  peer->owner = provider;
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
  if (peer->offer_started)
    return H2_PAL_ERR_INVALID_STATE;
  peer->offer_started = 1;
  char *sdp = NULL;
  size_t sdp_len = 0u;
  h2_pal_result_t result =
      (h2_pal_result_t)h2PionGoPeerStartOffer(peer->go_handle, &sdp, &sdp_len);
  if (result == H2_PAL_OK) {
    result = h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_LOCAL_SDP, NULL, 0,
                                   0, H2_PAL_WEBRTC_SDP_OFFER, sdp, sdp_len,
                                   0);
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
                                         int timeout_ms,
                                         h2_pal_webrtc_event_t *out_event) {
  if (peer == NULL || peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  if (timeout_ms < 0) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t queued = h2_pion_dequeue_event(peer, out_event);
  if (queued == H2_PAL_OK)
    return H2_PAL_OK;
  h2_pal_webrtc_track_t *track = peer->media_track;
  if (track != NULL && track->vtable != NULL && track->vtable->read != NULL &&
      peer->state == H2_PAL_WEBRTC_PEER_CONNECTED) {
    if (peer->pending_opus_len == 0u) {
      size_t opus_len = 0u;
      h2_pal_result_t read_result =
          track->vtable->read(track->user, peer->pending_opus,
                              sizeof(peer->pending_opus), &opus_len);
      if (read_result != H2_PAL_OK && read_result != H2_PAL_ERR_WOULD_BLOCK &&
          read_result != H2_PAL_ERR_TIMEOUT) {
        return read_result;
      }
      if (read_result == H2_PAL_OK) {
        if (opus_len == 0u || opus_len > sizeof(peer->pending_opus))
          return H2_PAL_ERR_FORMAT;
        peer->pending_opus_len = opus_len;
      }
    }
    if (peer->pending_opus_len != 0u) {
      h2_pal_result_t send_result =
          h2_pion_submit_opus(peer, peer->pending_opus, peer->pending_opus_len);
      if (send_result == H2_PAL_OK)
        peer->pending_opus_len = 0u;
      else if (send_result != H2_PAL_ERR_WOULD_BLOCK)
        return send_result;
    }
  }
  h2_pion_begin_operation(peer);
  h2_pal_result_t result = (h2_pal_result_t)h2PionGoPeerPoll(
      peer->go_handle, (uintptr_t)peer, timeout_ms);
  if (result == H2_PAL_OK && peer->media_result != H2_PAL_OK) {
    result = peer->media_result;
    peer->media_result = H2_PAL_OK;
  }
  if ((peer->closed || peer->owner->destroy_pending) && result == H2_PAL_OK) {
    result = H2_PAL_ERR_CLOSED;
  }
  result = h2_pion_end_operation(peer, result);
  if (result != H2_PAL_OK)
    return result;
  return h2_pion_dequeue_event(peer, out_event);
}

static h2_pal_result_t h2_pion_peer_set_track(h2_pal_webrtc_peer_t *peer,
                             h2_pal_webrtc_track_t *track) {
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (peer->offer_started)
    return H2_PAL_ERR_INVALID_STATE;
  if (track == NULL || track->vtable == NULL ||
      (track->vtable->read == NULL && track->vtable->write == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (peer->media_track != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  peer->media_track = track;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_pion_peer_unset_track(h2_pal_webrtc_peer_t *peer,
                                                h2_pal_webrtc_track_t *track) {
  if (peer == NULL || track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (peer->media_track != track)
    return H2_PAL_ERR_INVALID_STATE;
  peer->media_track = NULL;
  peer->pending_opus_len = 0u;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_pion_peer_send_opus(h2_pal_webrtc_peer_t *peer,
                                              const uint8_t *opus,
                                              size_t opus_len) {
  if (peer == NULL || peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  return h2_pion_submit_opus(peer, opus, opus_len);
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
  (void)h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, channel,
                              H2_PAL_WEBRTC_CHANNEL_CLOSED, 0, 0, NULL, 0u, 0);
}

static void h2_pion_peer_close_now(h2_pal_webrtc_peer_t *peer) {
  h2_pion_t *provider = peer->owner;
  peer->closed = 1;
  peer->close_pending = 0;
  h2PionGoPeerDestroy(peer->go_handle);
  peer->go_handle = 0u;
    peer->media_track = NULL;
  while (peer->event_head != NULL) {
    h2_pion_event_t *event = peer->event_head;
    peer->event_head = event->next;
    h2_pal_webrtc_event_t public_event = event->event;
    public_event._private = event;
    public_event._release = h2_pion_event_release;
    h2_pion_event_release(&public_event);
  }
  peer->event_tail = NULL;
  while (peer->channels != NULL) {
    h2_pal_webrtc_channel_t *channel = peer->channels;
    peer->channels = channel->next;
    channel->terminal = 1;
    h2_pion_free_channel(channel);
  }
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
    .peer_set_track = h2_pion_peer_set_track,
    .peer_unset_track = h2_pion_peer_unset_track,
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

#if H2_PION_TESTING
void h2_pion_test_block_next_opus_send(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL)
    return;
  peer->test_opus_send_attempts = 0u;
  peer->test_first_opus_len = 0u;
  peer->test_first_opus_hash = 0u;
  peer->test_opus_payload_mismatch = 0;
  peer->test_block_next_opus_send = 1;
}

size_t h2_pion_test_opus_send_attempts(h2_pal_webrtc_peer_t *peer) {
  return peer == NULL ? 0u : peer->test_opus_send_attempts;
}

int h2_pion_test_opus_send_payloads_match(h2_pal_webrtc_peer_t *peer) {
  return peer != NULL && peer->test_opus_send_attempts != 0u &&
         !peer->test_opus_payload_mismatch;
}
#endif

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
  if (channel != NULL && !channel->terminal)
    return h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE,
                                 channel, H2_PAL_WEBRTC_CHANNEL_OPEN, 0, 0,
                                 NULL, 0u, 0);
  return peer->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
}

void h2_pion_bridge_emit_channel_state(uintptr_t peer_key, uint64_t channel_key,
                                       int state) {
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
  (void)h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, channel,
                              (h2_pal_webrtc_channel_state_t)state, 0, 0, NULL,
                              0u, 0);
}

void h2_pion_bridge_emit_channel_message(uintptr_t peer_key,
                                         uint64_t channel_key,
                                         const uint8_t *data, size_t len,
                                         int is_text) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed || peer->close_pending ||
      peer->owner->destroy_pending) {
    return;
  }
  h2_pal_webrtc_channel_t *channel = h2_pion_find_channel(peer, channel_key);
  if (channel != NULL && !channel->terminal) {
    (void)h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE,
                                channel, 0, 0, 0, data, len, is_text);
  }
}

void h2_pion_bridge_emit_opus_frame(uintptr_t peer_key, const uint8_t *data,
                                    size_t len) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer != NULL && !peer->closed && !peer->close_pending &&
      !peer->owner->destroy_pending && peer->media_track != NULL &&
      peer->media_track->vtable != NULL &&
      peer->media_track->vtable->write != NULL) {
    const h2_pal_result_t result =
        peer->media_track->vtable->write(peer->media_track->user, data, len);
    if (peer->media_result == H2_PAL_OK && result != H2_PAL_OK)
      peer->media_result = result;
  } else if (peer != NULL && !peer->closed && !peer->close_pending &&
             !peer->owner->destroy_pending)
    (void)h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_OPUS_FRAME, NULL, 0,
                                0, 0, data, len, 0);
}
