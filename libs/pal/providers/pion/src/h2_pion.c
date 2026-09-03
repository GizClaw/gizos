#include "h2_pion.h"

#include "h2_pion_bridge.h"
#include "libs/pal/providers/pion/pion_archive.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef H2_PION_TESTING
#define H2_PION_TESTING 0
#endif

#define H2_PION_EVENT_LIMIT 256u
#define H2_PION_EVENT_BYTES (4u * 1024u * 1024u)

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
  size_t event_count;
  size_t event_bytes;
  h2_pal_mutex_t *mutex;
  h2_pal_semaphore_t *ready;
  h2_pal_task_t *worker;
  atomic_int stop;
  atomic_int worker_error;
  struct h2_pal_webrtc_channel *channels;
  uint64_t go_handle;
  uint64_t next_channel_key;
  h2_pal_webrtc_peer_state_t state;
  h2_pal_result_t media_result;
  int error_reported;
  uint8_t pending_opus[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
  size_t pending_opus_len;
  int offer_started;
  int closed;
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
  h2_pal_sync_api_t sync;
  h2_pal_task_api_t task;
  h2_pal_time_api_t time;
  h2_pal_webrtc_api_t api;
  struct h2_pal_webrtc_peer *peers;
  int destroy_pending;
};

static h2_pal_result_t h2_pion_peer_close_now(h2_pal_webrtc_peer_t *peer);
static void h2_pion_worker(void *context);
static _Thread_local h2_pal_webrtc_peer_t *s_worker_peer;

static h2_pal_result_t h2_pion_lock(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_worker_peer == peer)
    return H2_PAL_ERR_INVALID_STATE;
  return h2_pal_mutex_lock(&peer->owner->sync, peer->mutex);
}

static h2_pal_result_t h2_pion_unlock(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_result_t result) {
  const h2_pal_result_t rc =
      h2_pal_mutex_unlock(&peer->owner->sync, peer->mutex);
  return rc == H2_PAL_OK ? result : rc;
}

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
    h2_pal_webrtc_peer_state_t peer_state, h2_pal_webrtc_sdp_type_t sdp_type,
    const void *payload, size_t payload_len, int is_text) {
  if (peer == NULL || (payload == NULL && payload_len != 0u) ||
      payload_len == SIZE_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  if (payload_len > H2_PION_EVENT_BYTES)
    return H2_PAL_ERR_NO_SPACE;
  if (peer->event_count >= H2_PION_EVENT_LIMIT ||
      peer->event_bytes > H2_PION_EVENT_BYTES - payload_len)
    return H2_PAL_ERR_WOULD_BLOCK;
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
  peer->event_count++;
  peer->event_bytes += payload_len;
  (void)h2_pal_semaphore_give(&peer->owner->sync, peer->ready);
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
  peer->event_count--;
  peer->event_bytes -= node->event.kind == H2_PAL_WEBRTC_EVENT_LOCAL_SDP
                           ? node->event.sdp.len
                           : node->event.data_len;
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

static h2_pal_result_t
h2_pion_emit_peer_state(h2_pal_webrtc_peer_t *peer,
                        h2_pal_webrtc_peer_state_t state) {
  if (peer->state == state)
    return H2_PAL_OK;
  h2_pal_result_t rc = h2_pion_enqueue_event(
      peer, H2_PAL_WEBRTC_EVENT_PEER_STATE, NULL, 0, state, 0, NULL, 0u, 0);
  if (rc == H2_PAL_OK)
    peer->state = state;
  return rc;
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
  atomic_init(&peer->stop, 0);
  atomic_init(&peer->worker_error, H2_PAL_OK);
  const h2_pal_mutex_config_t mutex_config = {.name = "pion/peer",
                                              .allocator = &provider->mem};
  const h2_pal_semaphore_config_t ready_config = {
      .name = "pion/events", .allocator = &provider->mem, .max_count = 1u};
  h2_pal_result_t rc =
      h2_pal_mutex_create(&provider->sync, &mutex_config, &peer->mutex);
  if (rc == H2_PAL_OK)
    rc = h2_pal_semaphore_create(&provider->sync, &ready_config, &peer->ready);
  if (rc != H2_PAL_OK) {
    if (peer->mutex != NULL)
      (void)h2_pal_mutex_destroy(&provider->sync, peer->mutex);
    h2_pal_mem_free(&provider->mem, peer);
    return rc;
  }
  peer->next_channel_key = 1u;
  peer->state = H2_PAL_WEBRTC_PEER_NEW;
  peer->go_handle = h2PionGoPeerCreate();
  const h2_pal_task_options_t options = {.name = "pion/worker",
                                         .min_stack_size = 65536u};
  rc = peer->go_handle == 0u
           ? H2_PAL_ERR_IO
           : h2_pal_task_start(&provider->task, &options, h2_pion_worker, peer,
                               &peer->worker);
  if (rc != H2_PAL_OK) {
    if (peer->go_handle != 0u)
      h2PionGoPeerDestroy(peer->go_handle);
    (void)h2_pal_semaphore_destroy(&provider->sync, peer->ready);
    (void)h2_pal_mutex_destroy(&provider->sync, peer->mutex);
    h2_pal_mem_free(&provider->mem, peer);
    return rc;
  }
  peer->next = provider->peers;
  provider->peers = peer;
  *out_peer = peer;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_pion_peer_add_ice_server_locked(h2_pal_webrtc_peer_t *peer,
                                   const h2_pal_webrtc_ice_server_t *server) {
  if (peer == NULL || server == NULL || peer->closed || peer->offer_started) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return (h2_pal_result_t)h2PionGoPeerAddICEServer(
      peer->go_handle, (char *)server->url.data, server->url.len,
      (char *)server->username.data, server->username.len,
      (char *)server->credential.data, server->credential.len);
}

static h2_pal_result_t
h2_pion_peer_start_offer_locked(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL || peer->closed) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (peer->offer_started)
    return H2_PAL_ERR_INVALID_STATE;
  // Do not start irreversible signaling when its SDP cannot be queued. The
  // public caller may retry WOULD_BLOCK after draining events.
  if (peer->event_count >= H2_PION_EVENT_LIMIT || peer->event_bytes != 0u)
    return H2_PAL_ERR_WOULD_BLOCK;
  peer->offer_started = 1;
  char *sdp = NULL;
  size_t sdp_len = 0u;
  h2_pal_result_t result =
      (h2_pal_result_t)h2PionGoPeerStartOffer(peer->go_handle, &sdp, &sdp_len);
  if (result == H2_PAL_OK) {
    result = h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_LOCAL_SDP, NULL, 0,
                                   0, H2_PAL_WEBRTC_SDP_OFFER, sdp, sdp_len, 0);
  }
  free(sdp);
  return result;
}

static h2_pal_result_t
h2_pion_peer_set_remote_sdp_locked(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_sdp_type_t type,
                                   h2_pal_webrtc_str_t sdp) {
  if (peer == NULL || peer->closed || type != H2_PAL_WEBRTC_SDP_ANSWER) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return (h2_pal_result_t)h2PionGoPeerSetRemoteSDP(peer->go_handle,
                                                   (char *)sdp.data, sdp.len);
}

static h2_pal_result_t h2_pion_peer_create_data_channel_locked(
    h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_channel_config_t *config,
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

static h2_pal_result_t h2_pion_service_track(h2_pal_webrtc_peer_t *peer) {
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
  return H2_PAL_OK;
}

static void h2_pion_worker(void *context) {
  h2_pal_webrtc_peer_t *peer = context;
  h2_pion_t *owner = peer->owner;
  s_worker_peer = peer;
  while (!atomic_load(&peer->stop)) {
    h2_pal_result_t worker_rc = h2_pal_mutex_lock(&owner->sync, peer->mutex);
    if (worker_rc != H2_PAL_OK) {
      atomic_store(&peer->worker_error, worker_rc);
      break;
    }
    if (!peer->closed && peer->media_result == H2_PAL_OK) {
      h2_pal_result_t rc = h2_pion_service_track(peer);
      if (rc == H2_PAL_OK)
        rc = (h2_pal_result_t)h2PionGoPeerDispatch(peer->go_handle,
                                                   (uintptr_t)peer);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK &&
          rc != H2_PAL_ERR_TIMEOUT) {
        peer->media_result = rc;
        (void)h2_pal_semaphore_give(&owner->sync, peer->ready);
      }
    }
    worker_rc = h2_pal_mutex_unlock(&owner->sync, peer->mutex);
    if (worker_rc == H2_PAL_OK)
      worker_rc = h2_pal_time_sleep_ms(&owner->time, 1u);
    if (worker_rc != H2_PAL_OK) {
      atomic_store(&peer->worker_error, worker_rc);
      break;
    }
  }
  (void)h2_pal_semaphore_give(&owner->sync, peer->ready);
  s_worker_peer = NULL;
}

static h2_pal_result_t h2_pion_peer_poll(h2_pal_webrtc_peer_t *peer,
                                         int timeout_ms,
                                         h2_pal_webrtc_event_t *out_event) {
  if (peer == NULL || out_event == NULL || timeout_ms < 0)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_event, 0, sizeof(*out_event));
  uint64_t start = 0u, now = 0u;
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(&peer->owner->time, &start);
  if (rc != H2_PAL_OK)
    return rc;
  for (;;) {
    rc = h2_pion_lock(peer);
    if (rc != H2_PAL_OK)
      return rc;
    rc = peer->closed ? H2_PAL_ERR_CLOSED
                      : h2_pion_dequeue_event(peer, out_event);
    const h2_pal_result_t failure =
        peer->media_result != H2_PAL_OK
            ? peer->media_result
            : (h2_pal_result_t)atomic_load(&peer->worker_error);
    if (rc == H2_PAL_ERR_WOULD_BLOCK && failure != H2_PAL_OK) {
      rc = failure;
      if (!peer->error_reported) {
        rc = h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_ERROR, NULL, 0, 0,
                                   0, NULL, 0u, 0);
        if (rc == H2_PAL_OK) {
          peer->event_tail->event.error = failure;
          peer->error_reported = 1;
          rc = h2_pion_dequeue_event(peer, out_event);
        }
      }
    }
    rc = h2_pion_unlock(peer, rc);
    if (rc != H2_PAL_ERR_WOULD_BLOCK || timeout_ms == 0)
      return rc;
    rc = h2_pal_time_get_monotonic_ms(&peer->owner->time, &now);
    if (rc != H2_PAL_OK)
      return rc;
    if (now - start >= (uint64_t)timeout_ms)
      return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_semaphore_take(&peer->owner->sync, peer->ready,
                               (uint32_t)(timeout_ms - (now - start)));
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT)
      return rc;
  }
}

static h2_pal_result_t
h2_pion_peer_set_track_locked(h2_pal_webrtc_peer_t *peer,
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

static h2_pal_result_t
h2_pion_peer_unset_track_locked(h2_pal_webrtc_peer_t *peer,
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

static h2_pal_result_t h2_pion_peer_send_opus_locked(h2_pal_webrtc_peer_t *peer,
                                                     const uint8_t *opus,
                                                     size_t opus_len) {
  if (peer == NULL || peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  return h2_pion_submit_opus(peer, opus, opus_len);
}

static h2_pal_result_t
h2_pion_channel_send_locked(h2_pal_webrtc_channel_t *channel,
                            const uint8_t *data, size_t len, int is_text) {
  if (channel == NULL || channel->terminal || channel->peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  return (h2_pal_result_t)h2PionGoChannelSend(
      channel->peer->go_handle, channel->key, (uint8_t *)data, len, is_text);
}

static void h2_pion_channel_close_locked(h2_pal_webrtc_channel_t *channel) {
  if (channel == NULL || channel->terminal) {
    return;
  }
  h2_pal_webrtc_peer_t *peer = channel->peer;
  channel->terminal = 1;
  h2PionGoChannelClose(peer->go_handle, channel->key);
  const h2_pal_result_t rc =
      h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, channel,
                            H2_PAL_WEBRTC_CHANNEL_CLOSED, 0, 0, NULL, 0u, 0);
  if (rc != H2_PAL_OK) {
    peer->media_result = rc == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_ERR_FULL : rc;
    (void)h2_pal_semaphore_give(&peer->owner->sync, peer->ready);
  }
}

static h2_pal_result_t h2_pion_peer_close_now(h2_pal_webrtc_peer_t *peer) {
  h2_pion_t *provider = peer->owner;
  if (s_worker_peer == peer)
    return H2_PAL_ERR_INVALID_STATE;
  atomic_store(&peer->stop, 1);
  // Even a failed join must not leave the caller's Track borrowed. Acquiring
  // the same mutex drains in-flight read/write before detaching it.
  h2_pal_result_t lock_rc = h2_pion_lock(peer);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  peer->closed = 1;
  peer->media_track = NULL;
  peer->pending_opus_len = 0u;
  lock_rc = h2_pion_unlock(peer, H2_PAL_OK);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  if (peer->worker != NULL) {
    h2_pal_result_t rc = h2_pal_task_join(&provider->task, peer->worker);
    if (rc != H2_PAL_OK)
      return rc;
    peer->worker = NULL;
  }
  h2PionGoPeerDestroy(peer->go_handle);
  peer->go_handle = 0u;
  h2_pal_webrtc_event_t event = {0};
  while (h2_pion_dequeue_event(peer, &event) == H2_PAL_OK)
    h2_pion_event_release(&event);
  while (peer->channels != NULL) {
    h2_pal_webrtc_channel_t *channel = peer->channels;
    peer->channels = channel->next;
    h2_pion_free_channel(channel);
  }
  h2_pal_webrtc_peer_t **cursor = &provider->peers;
  while (*cursor != NULL && *cursor != peer)
    cursor = &(*cursor)->next;
  if (*cursor == peer)
    *cursor = peer->next;
  (void)h2_pal_semaphore_destroy(&provider->sync, peer->ready);
  (void)h2_pal_mutex_destroy(&provider->sync, peer->mutex);
  h2_pal_mem_free(&provider->mem, peer);
  return H2_PAL_OK;
}

static void h2_pion_peer_close(h2_pal_webrtc_peer_t *peer) {
  if (peer != NULL)
    (void)h2_pion_peer_close_now(peer);
}

static h2_pal_result_t
h2_pion_peer_add_ice_server(h2_pal_webrtc_peer_t *peer,
                            const h2_pal_webrtc_ice_server_t *server) {
  h2_pal_webrtc_peer_t *target = peer;
  h2_pal_result_t rc = h2_pion_lock(target);
  if (rc != H2_PAL_OK)
    return rc;
  rc = atomic_load(&target->stop)
           ? H2_PAL_ERR_CLOSED
           : h2_pion_peer_add_ice_server_locked(peer, server);
  return h2_pion_unlock(target, rc);
}

static h2_pal_result_t h2_pion_peer_start_offer(h2_pal_webrtc_peer_t *peer) {
  h2_pal_webrtc_peer_t *target = peer;
  h2_pal_result_t rc = h2_pion_lock(target);
  if (rc != H2_PAL_OK)
    return rc;
  rc = atomic_load(&target->stop) ? H2_PAL_ERR_CLOSED
                                  : h2_pion_peer_start_offer_locked(peer);
  return h2_pion_unlock(target, rc);
}

static h2_pal_result_t
h2_pion_peer_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                            h2_pal_webrtc_sdp_type_t type,
                            h2_pal_webrtc_str_t sdp) {
  h2_pal_webrtc_peer_t *target = peer;
  h2_pal_result_t rc = h2_pion_lock(target);
  if (rc != H2_PAL_OK)
    return rc;
  rc = atomic_load(&target->stop)
           ? H2_PAL_ERR_CLOSED
           : h2_pion_peer_set_remote_sdp_locked(peer, type, sdp);
  return h2_pion_unlock(target, rc);
}

static h2_pal_result_t
h2_pion_peer_create_data_channel(h2_pal_webrtc_peer_t *peer,
                                 const h2_pal_webrtc_channel_config_t *config,
                                 h2_pal_webrtc_channel_t **out_channel) {
  h2_pal_webrtc_peer_t *target = peer;
  h2_pal_result_t rc = h2_pion_lock(target);
  if (rc != H2_PAL_OK)
    return rc;
  rc = atomic_load(&target->stop)
           ? H2_PAL_ERR_CLOSED
           : h2_pion_peer_create_data_channel_locked(peer, config, out_channel);
  return h2_pion_unlock(target, rc);
}

static h2_pal_result_t h2_pion_peer_set_track(h2_pal_webrtc_peer_t *peer,
                                              h2_pal_webrtc_track_t *track) {
  h2_pal_webrtc_peer_t *target = peer;
  h2_pal_result_t rc = h2_pion_lock(target);
  if (rc != H2_PAL_OK)
    return rc;
  rc = atomic_load(&target->stop) ? H2_PAL_ERR_CLOSED
                                  : h2_pion_peer_set_track_locked(peer, track);
  return h2_pion_unlock(target, rc);
}

static h2_pal_result_t h2_pion_peer_unset_track(h2_pal_webrtc_peer_t *peer,
                                                h2_pal_webrtc_track_t *track) {
  h2_pal_webrtc_peer_t *target = peer;
  h2_pal_result_t rc = h2_pion_lock(target);
  if (rc != H2_PAL_OK)
    return rc;
  rc = atomic_load(&target->stop)
           ? H2_PAL_ERR_CLOSED
           : h2_pion_peer_unset_track_locked(peer, track);
  return h2_pion_unlock(target, rc);
}

static h2_pal_result_t h2_pion_peer_send_opus(h2_pal_webrtc_peer_t *peer,
                                              const uint8_t *opus,
                                              size_t opus_len) {
  h2_pal_webrtc_peer_t *target = peer;
  h2_pal_result_t rc = h2_pion_lock(target);
  if (rc != H2_PAL_OK)
    return rc;
  rc = atomic_load(&target->stop)
           ? H2_PAL_ERR_CLOSED
           : h2_pion_peer_send_opus_locked(peer, opus, opus_len);
  return h2_pion_unlock(target, rc);
}

static h2_pal_result_t h2_pion_channel_send(h2_pal_webrtc_channel_t *channel,
                                            const uint8_t *data, size_t len,
                                            int is_text) {
  h2_pal_webrtc_peer_t *target = channel == NULL ? NULL : channel->peer;
  h2_pal_result_t rc = h2_pion_lock(target);
  if (rc != H2_PAL_OK)
    return rc;
  rc = atomic_load(&target->stop)
           ? H2_PAL_ERR_CLOSED
           : h2_pion_channel_send_locked(channel, data, len, is_text);
  return h2_pion_unlock(target, rc);
}

static void h2_pion_channel_close(h2_pal_webrtc_channel_t *channel) {
  if (channel == NULL || h2_pion_lock(channel->peer) != H2_PAL_OK)
    return;
  h2_pal_webrtc_peer_t *peer = channel->peer;
  h2_pion_channel_close_locked(channel);
  (void)h2_pion_unlock(peer, H2_PAL_OK);
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
  if (config->sync == NULL || config->sync->vtable == NULL ||
      config->sync->vtable->create_mutex == NULL ||
      config->sync->vtable->destroy_mutex == NULL ||
      config->sync->vtable->lock_mutex == NULL ||
      config->sync->vtable->unlock_mutex == NULL ||
      config->sync->vtable->create_semaphore == NULL ||
      config->sync->vtable->destroy_semaphore == NULL ||
      config->sync->vtable->take_semaphore == NULL ||
      config->sync->vtable->give_semaphore == NULL || config->task == NULL ||
      config->task->vtable == NULL || config->task->vtable->start == NULL ||
      config->task->vtable->join == NULL || config->time == NULL ||
      config->time->vtable == NULL || config->time->vtable->sleep_ms == NULL ||
      config->time->vtable->get_monotonic_ms == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pion_t temporary = {.mem = *config->mem};
  h2_pion_t *provider = h2_pion_alloc(&temporary, sizeof(*provider));
  if (provider == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  provider->mem = *config->mem;
  provider->sync = *config->sync;
  provider->task = *config->task;
  provider->time = *config->time;
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
  if (h2_pion_lock(peer) != H2_PAL_OK)
    return;
  peer->test_opus_send_attempts = 0u;
  peer->test_first_opus_len = 0u;
  peer->test_first_opus_hash = 0u;
  peer->test_opus_payload_mismatch = 0;
  peer->test_block_next_opus_send = 1;
  (void)h2_pion_unlock(peer, H2_PAL_OK);
}

size_t h2_pion_test_opus_send_attempts(h2_pal_webrtc_peer_t *peer) {
  if (h2_pion_lock(peer) != H2_PAL_OK)
    return 0u;
  size_t attempts = peer->test_opus_send_attempts;
  (void)h2_pion_unlock(peer, H2_PAL_OK);
  return attempts;
}

int h2_pion_test_opus_send_payloads_match(h2_pal_webrtc_peer_t *peer) {
  if (h2_pion_lock(peer) != H2_PAL_OK)
    return 0;
  int matches =
      peer->test_opus_send_attempts != 0u && !peer->test_opus_payload_mismatch;
  (void)h2_pion_unlock(peer, H2_PAL_OK);
  return matches;
}

h2_pal_result_t h2_pion_test_connected(h2_pal_webrtc_peer_t *peer) {
  h2_pal_result_t rc = h2_pion_lock(peer);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_pion_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CONNECTED);
  return h2_pion_unlock(peer, rc);
}

h2_pal_result_t h2_pion_test_remote_channel(h2_pal_webrtc_peer_t *peer) {
  h2_pal_result_t rc = h2_pion_lock(peer);
  if (rc != H2_PAL_OK)
    return rc;
  rc = (h2_pal_result_t)h2_pion_bridge_emit_channel_open(
      (uintptr_t)peer, 99u, "remote", 6u, 1, 2u, 1, 1, 1);
  return h2_pion_unlock(peer, rc);
}
#endif

void h2_pion_destroy(h2_pion_t **provider_ptr) {
  if (provider_ptr == NULL || *provider_ptr == NULL)
    return;
  h2_pion_t *provider = *provider_ptr;
  if (s_worker_peer != NULL && s_worker_peer->owner == provider)
    return;
  provider->destroy_pending = 1;
  while (provider->peers != NULL) {
    if (h2_pion_peer_close_now(provider->peers) != H2_PAL_OK)
      return;
  }
  h2_pal_mem_api_t mem = provider->mem;
  h2_pal_mem_free(&mem, provider);
  *provider_ptr = NULL;
}

/* Called synchronously by the worker's Go bridge while peer->mutex is held. */
int h2_pion_bridge_emit_writable(uintptr_t peer_key, uint64_t channel_key) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  h2_pal_webrtc_channel_t *channel = h2_pion_find_channel(peer, channel_key);
  if (channel == NULL || channel->terminal)
    return H2_PAL_OK;
  return h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_WRITABLE, channel, 0,
                               0, 0, NULL, 0u, 0);
}

int h2_pion_bridge_emit_peer_state(uintptr_t peer_key, int state) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed || state < H2_PAL_WEBRTC_PEER_NEW ||
      state > H2_PAL_WEBRTC_PEER_CLOSED)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_pion_emit_peer_state(peer, (h2_pal_webrtc_peer_state_t)state);
}

int h2_pion_bridge_emit_channel_open(uintptr_t peer_key, uint64_t channel_key,
                                     const char *label, size_t label_len,
                                     int has_stream_id, uint16_t stream_id,
                                     int ordered, int reliable, int remote) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  h2_pal_webrtc_channel_t *channel = h2_pion_find_channel(peer, channel_key);
  if (channel == NULL && remote) {
    channel = h2_pion_new_channel(peer, channel_key, label, label_len,
                                  has_stream_id, stream_id, ordered, reliable);
    if (channel == NULL)
      return H2_PAL_ERR_NO_MEMORY;
  }
  if (channel != NULL && !channel->terminal) {
    channel->info.has_stream_id = has_stream_id != 0;
    channel->info.stream_id = stream_id;
    return h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE,
                                 channel, H2_PAL_WEBRTC_CHANNEL_OPEN, 0, 0,
                                 NULL, 0u, 0);
  }
  return H2_PAL_OK;
}

int h2_pion_bridge_emit_channel_state(uintptr_t peer_key, uint64_t channel_key,
                                      int state) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (state != H2_PAL_WEBRTC_CHANNEL_CLOSED &&
      state != H2_PAL_WEBRTC_CHANNEL_ERROR)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_webrtc_channel_t *channel = h2_pion_find_channel(peer, channel_key);
  if (channel == NULL || channel->terminal)
    return H2_PAL_OK;
  h2_pal_result_t rc = h2_pion_enqueue_event(
      peer, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, channel,
      (h2_pal_webrtc_channel_state_t)state, 0, 0, NULL, 0u, 0);
  if (rc == H2_PAL_OK)
    channel->terminal = 1;
  return rc;
}

int h2_pion_bridge_emit_channel_message(uintptr_t peer_key,
                                        uint64_t channel_key,
                                        const uint8_t *data, size_t len,
                                        int is_text) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  h2_pal_webrtc_channel_t *channel = h2_pion_find_channel(peer, channel_key);
  if (channel == NULL || channel->terminal)
    return H2_PAL_OK;
  return h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE,
                               channel, 0, 0, 0, data, len, is_text);
}

int h2_pion_bridge_emit_opus_frame(uintptr_t peer_key, const uint8_t *data,
                                   size_t len) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)peer_key;
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  h2_pal_webrtc_track_t *track = peer->media_track;
  if (track != NULL && track->vtable->write != NULL)
    return track->vtable->write(track->user, data, len);
  return h2_pion_enqueue_event(peer, H2_PAL_WEBRTC_EVENT_OPUS_FRAME, NULL, 0, 0,
                               0, data, len, 0);
}
