#include "h2_peer_internal.h"
#include "h2_peer_task_names.h"

#include "ice/h2_peer_ice.h"
#include "providers/h2_peer_portable_backend.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_PEER_TX_RESERVED ((h2_peer_tx_item_t *)(uintptr_t)1u)

enum {
  H2_PEER_NETWORK_COMMAND_COUNT = 1,
  H2_PEER_NETWORK_EVENT_HIGH_WATER = 12,
  H2_PEER_NETWORK_EVENT_COUNT =
      H2_PEER_STREAM_COUNT + H2_PEER_NETWORK_EVENT_HIGH_WATER + 4,
  H2_PEER_NETWORK_EVENT_BYTES_HIGH_WATER = 16 * 1024,
  H2_PEER_NETWORK_POLL_MS = 2,
  H2_PEER_NETWORK_IDLE_WAIT_MS = 1,
  H2_PEER_NETWORK_UDP_BURST_MAX = 16,
  H2_PEER_NETWORK_STACK_SIZE = 32 * 1024,
};

static void h2_peer_media_log(h2_pal_webrtc_peer_t *peer,
                              h2_pal_log_level_t level, const char *scope,
                              const char *message) {
  if (peer != NULL && peer->owner != NULL && peer->owner->config.log != NULL &&
      peer->owner->config.log->vtable != NULL &&
      peer->owner->config.log->vtable->write != NULL)
    (void)h2_pal_log_write(peer->owner->config.log, level, scope, message);
}

typedef enum h2_peer_network_command_type {
  H2_PEER_NETWORK_ADD_ICE_SERVER = 1,
  H2_PEER_NETWORK_START_OFFER,
  H2_PEER_NETWORK_SET_REMOTE_SDP,
  H2_PEER_NETWORK_CREATE_DATA_CHANNEL,
  H2_PEER_NETWORK_SET_MEDIA_TRACK,
  H2_PEER_NETWORK_UNSET_MEDIA_TRACK,
  H2_PEER_NETWORK_CHANNEL_CLOSE,
  H2_PEER_NETWORK_PEER_CLOSE,
} h2_peer_network_command_type_t;

typedef struct h2_peer_network_command {
  h2_peer_network_command_type_t type;
  union {
    const h2_pal_webrtc_ice_server_t *ice_server;
    struct {
      h2_pal_webrtc_sdp_type_t type;
      h2_pal_webrtc_str_t sdp;
    } remote_sdp;
    const h2_pal_webrtc_channel_config_t *channel_config;
    h2_pal_webrtc_track_t *media_track;
    h2_pal_webrtc_channel_t *channel;
  } value;
} h2_peer_network_command_t;

typedef struct h2_peer_network_response {
  h2_pal_result_t result;
  h2_pal_webrtc_channel_t *channel;
} h2_peer_network_response_t;

typedef enum h2_peer_network_event_type {
  H2_PEER_NETWORK_EVENT_PEER_STATE = 1,
  H2_PEER_NETWORK_EVENT_LOCAL_SDP,
  H2_PEER_NETWORK_EVENT_CHANNEL_STATE,
  H2_PEER_NETWORK_EVENT_CHANNEL_MESSAGE,
  H2_PEER_NETWORK_EVENT_OPUS_FRAME,
  H2_PEER_NETWORK_EVENT_SEND_READY,
  H2_PEER_NETWORK_EVENT_ERROR,
} h2_peer_network_event_type_t;

typedef struct h2_peer_network_event {
  h2_peer_network_event_type_t type;
  h2_pal_webrtc_peer_t *peer;
  h2_pal_webrtc_channel_t *channel;
  h2_pal_webrtc_peer_state_t peer_state;
  h2_pal_webrtc_channel_state_t channel_state;
  h2_pal_webrtc_sdp_type_t sdp_type;
  uint8_t *data;
  size_t data_len;
  /* Byte count charged to network_event_bytes at enqueue. data_len is a view
   * that callers may reshape per kind, so release must refund this instead. */
  size_t owned_data_len;
  int is_text;
  h2_pal_result_t error;
} h2_peer_network_event_t;

static h2_pal_result_t
h2_peer_queue_network_event(h2_pal_webrtc_peer_t *peer,
                            h2_peer_network_event_t *event, const uint8_t *data,
                            size_t data_len);

static void *h2_peer_alloc(h2_peer_t *owner, size_t len) {
  void *ptr = h2_pal_mem_alloc(owner->config.mem, len);
  if (ptr != NULL) {
    memset(ptr, 0, len);
  }
  return ptr;
}

static void h2_peer_free(h2_peer_t *owner, void *ptr) {
  h2_pal_mem_free(owner->config.mem, ptr);
}

static const h2_pal_mem_api_t *h2_peer_control_mem(const h2_peer_t *owner) {
  return owner->config.control_mem != NULL ? owner->config.control_mem
                                           : owner->config.mem;
}

static void *h2_peer_control_alloc(h2_peer_t *owner, size_t len) {
  void *ptr = h2_pal_mem_alloc(h2_peer_control_mem(owner), len);
  if (ptr != NULL) {
    memset(ptr, 0, len);
  }
  return ptr;
}

static void h2_peer_control_free(h2_peer_t *owner, void *ptr) {
  h2_pal_mem_free(h2_peer_control_mem(owner), ptr);
}

static void h2_peer_free_tx_item(h2_peer_t *owner, h2_peer_tx_item_t *item) {
  if (item == NULL || item == H2_PEER_TX_RESERVED) {
    return;
  }
  h2_peer_free(owner, item->data);
  h2_peer_free(owner, item);
}

static h2_peer_tx_item_t *h2_peer_prepare_tx_item(h2_peer_t *owner,
                                                  h2_peer_tx_item_t **storage,
                                                  const uint8_t *data,
                                                  size_t len, int is_text) {
  h2_peer_tx_item_t *item = *storage;
  if (item == NULL || item == H2_PEER_TX_RESERVED) {
    item = h2_peer_alloc(owner, sizeof(*item));
    if (item == NULL) {
      return NULL;
    }
    *storage = item;
  }
  if (len > item->capacity) {
    uint8_t *replacement = h2_peer_alloc(owner, len);
    if (replacement == NULL) {
      return NULL;
    }
    h2_peer_free(owner, item->data);
    item->data = replacement;
    item->capacity = len;
  }
  if (len != 0u) {
    memcpy(item->data, data, len);
  }
  item->len = len;
  item->is_text = is_text != 0;
  return item;
}

static void h2_peer_network_notify_send_ready(h2_pal_webrtc_peer_t *peer) {
  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &peer->network_send_wakeup_queued, &expected, 1, memory_order_acq_rel,
          memory_order_relaxed)) {
    return;
  }
  h2_peer_network_event_t event = {
      .type = H2_PEER_NETWORK_EVENT_SEND_READY,
  };
  if (h2_peer_queue_network_event(peer, &event, NULL, 0u) != H2_PAL_OK) {
    atomic_store_explicit(&peer->network_send_wakeup_queued, 0,
                          memory_order_release);
  }
}

static void h2_peer_channel_ready_set(h2_pal_webrtc_channel_t *channel);

static void h2_peer_channel_tx_init(h2_pal_webrtc_channel_t *channel) {
  for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i) {
    atomic_init(&channel->tx_state[i], 0u);
  }
  atomic_init(&channel->tx_head, 0u);
  atomic_init(&channel->tx_tail, 0u);
}

h2_pal_result_t h2_peer_channel_tx_push(h2_pal_webrtc_channel_t *channel,
                                        const uint8_t *data, size_t len,
                                        int is_text) {
  h2_pal_webrtc_peer_t *peer = channel->owner;
  const uint8_t slot =
      atomic_load_explicit(&channel->tx_tail, memory_order_acquire);
  unsigned char expected = 0u;
  // Only the claimant advances the tail, so a second producer racing for the
  // same slot sees it busy and reports WOULD_BLOCK; the ring never has holes.
  if (!atomic_compare_exchange_strong_explicit(
          &channel->tx_state[slot], &expected, 1u, memory_order_acq_rel,
          memory_order_relaxed)) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_peer_tx_item_t *item = h2_peer_prepare_tx_item(
      peer->owner, &channel->tx_storage[slot], data, len, is_text);
  if (item == NULL) {
    atomic_store_explicit(&channel->tx_state[slot], 0u, memory_order_release);
    return H2_PAL_ERR_NO_MEMORY;
  }
  atomic_store_explicit(&channel->tx_tail,
                        (uint8_t)((slot + 1u) % H2_PEER_INPUT_SLOT_COUNT),
                        memory_order_release);
  atomic_store_explicit(&channel->tx_state[slot], 2u, memory_order_release);
  h2_peer_channel_ready_set(channel);
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_channel_ready_slot_allocate(h2_pal_webrtc_channel_t *channel) {
  h2_pal_webrtc_peer_t *peer = channel->owner;
  for (uint8_t slot = 0u; slot < H2_PEER_READY_CHANNEL_COUNT; ++slot) {
    bool used = false;
    for (h2_pal_webrtc_channel_t *existing = peer->channels; existing != NULL;
         existing = existing->next) {
      if (existing->ready_slot == slot) {
        used = true;
        break;
      }
    }
    if (!used) {
      channel->ready_slot = slot;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NO_SPACE;
}

static void h2_peer_channel_ready_set(h2_pal_webrtc_channel_t *channel) {
  // A remote reset may retire the slot while a caller finishes enqueueing.
  // Read it once so validation and the shift use the same bounded value.
  const uint8_t slot =
      atomic_load_explicit(&channel->ready_slot, memory_order_acquire);
  if (slot >= H2_PEER_READY_CHANNEL_COUNT) {
    return;
  }
  atomic_fetch_or_explicit(&channel->owner->channel_ready, UINT32_C(1) << slot,
                           memory_order_release);
}

static void h2_peer_channel_ready_clear(h2_pal_webrtc_channel_t *channel) {
  const uint8_t slot = atomic_exchange_explicit(&channel->ready_slot, UINT8_MAX,
                                                memory_order_acq_rel);
  if (slot >= H2_PEER_READY_CHANNEL_COUNT) {
    return;
  }
  const uint32_t bit = UINT32_C(1) << slot;
  atomic_fetch_and_explicit(&channel->owner->channel_ready, ~bit,
                            memory_order_acq_rel);
}

static char *h2_peer_copy_string(h2_peer_t *owner, h2_pal_webrtc_str_t value) {
  if (value.len == 0u) {
    return NULL;
  }
  if (value.data == NULL || value.len == SIZE_MAX) {
    return NULL;
  }
  char *copy = (char *)h2_peer_alloc(owner, value.len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value.data, value.len);
  copy[value.len] = '\0';
  return copy;
}

static int h2_peer_config_valid(const h2_peer_config_t *config) {
  return config != NULL && config->mem != NULL && config->mem->vtable != NULL &&
         config->mem->vtable->alloc != NULL &&
         config->mem->vtable->free != NULL &&
         (config->control_mem == NULL ||
          (config->control_mem->vtable != NULL &&
           config->control_mem->vtable->alloc != NULL &&
           config->control_mem->vtable->free != NULL)) &&
         config->log != NULL && config->log->vtable != NULL &&
         config->log->vtable->write != NULL && config->net != NULL &&
         config->net->vtable != NULL && config->queue != NULL &&
         config->queue->vtable != NULL && config->sync != NULL &&
         config->sync->vtable != NULL && config->task != NULL &&
         config->task->vtable != NULL && config->time != NULL &&
         config->time->vtable != NULL &&
         config->time->vtable->get_monotonic_ms != NULL &&
         config->time->vtable->get_monotonic_us != NULL &&
         config->crypto != NULL && config->crypto->vtable != NULL &&
         config->crypto->vtable->random != NULL && config->dtls != NULL &&
         config->dtls->vtable != NULL && config->sctp != NULL &&
         config->sctp->vtable != NULL;
}

static int h2_peer_production_config_supported(const h2_peer_config_t *config) {
  const h2_pal_net_vtable_t *net = config->net->vtable;
  return net->resolve_addr != NULL && net->get_host_addr != NULL &&
         net->udp_open != NULL && net->udp_sendto != NULL &&
         net->udp_recvfrom != NULL && net->tcp_open != NULL &&
         net->tcp_open_bound != NULL && net->tcp_connect != NULL &&
         net->tcp_send_timeout != NULL && net->tcp_recv != NULL &&
         net->close != NULL && config->queue->vtable->create != NULL &&
         config->queue->vtable->destroy != NULL &&
         config->queue->vtable->send != NULL &&
         config->queue->vtable->send_latest != NULL &&
         config->queue->vtable->recv != NULL &&
         config->sync->vtable->create_mutex != NULL &&
         config->sync->vtable->destroy_mutex != NULL &&
         config->sync->vtable->lock_mutex != NULL &&
         config->sync->vtable->try_lock_mutex != NULL &&
         config->sync->vtable->unlock_mutex != NULL &&
         config->sync->vtable->create_cond != NULL &&
         config->sync->vtable->destroy_cond != NULL &&
         config->sync->vtable->wait_cond != NULL &&
         config->sync->vtable->broadcast_cond != NULL &&
         config->task->vtable->start != NULL &&
         config->task->vtable->join != NULL &&
         config->time->vtable->sleep_ms != NULL &&
         config->dtls->vtable->session_create != NULL &&
         config->dtls->vtable->session_get_local_fingerprint != NULL &&
         config->dtls->vtable->session_set_remote_fingerprint != NULL &&
         config->dtls->vtable->session_handshake != NULL &&
         config->dtls->vtable->session_next_deadline_ms != NULL &&
         config->dtls->vtable->session_flush != NULL &&
         config->dtls->vtable->session_get_srtp_profile != NULL &&
         config->dtls->vtable->session_export_srtp_keying_material != NULL &&
         config->dtls->vtable->session_write != NULL &&
         config->dtls->vtable->session_consume_datagram != NULL &&
         config->dtls->vtable->session_close != NULL &&
         config->dtls->vtable->session_destroy != NULL &&
         config->sctp->vtable->association_create != NULL &&
         config->sctp->vtable->association_start != NULL &&
         config->sctp->vtable->association_input_packet != NULL &&
         config->sctp->vtable->association_service != NULL &&
         config->sctp->vtable->association_send_message != NULL &&
         config->sctp->vtable->association_is_writable != NULL &&
         config->sctp->vtable->association_reset_stream != NULL &&
         config->sctp->vtable->association_shutdown != NULL &&
         config->sctp->vtable->association_abort != NULL &&
         config->sctp->vtable->association_close != NULL;
}

static void h2_peer_free_channel(h2_pal_webrtc_peer_t *peer,
                                 h2_pal_webrtc_channel_t *channel);
static void h2_peer_network_discard_available(h2_pal_webrtc_peer_t *peer);
static void h2_peer_network_destroy_resources(h2_pal_webrtc_peer_t *peer);

static void h2_peer_owner_release(h2_peer_t *owner) {
  if (atomic_fetch_sub_explicit(&owner->refs, 1u, memory_order_acq_rel) == 1u)
    h2_peer_control_free(owner, owner);
}

static void h2_peer_connection_release(h2_pal_webrtc_peer_t *peer) {
  if (atomic_fetch_sub_explicit(&peer->refs, 1u, memory_order_acq_rel) == 1u) {
    h2_peer_t *owner = peer->owner;
    h2_peer_control_free(owner, peer);
    h2_peer_owner_release(owner);
  }
}

static h2_pal_result_t
h2_peer_queue_network_event(h2_pal_webrtc_peer_t *peer,
                            h2_peer_network_event_t *event, const uint8_t *data,
                            size_t data_len) {
  if (data_len != 0u && data == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_peer_network_event_t *queued_event =
      h2_peer_alloc(peer->owner, sizeof(*queued_event) + data_len);
  if (queued_event == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  *queued_event = *event;
  queued_event->peer = peer;
  queued_event->owned_data_len = data_len;
  if (data_len != 0u) {
    queued_event->data = (uint8_t *)(queued_event + 1);
    memcpy(queued_event->data, data, data_len);
    queued_event->data_len = data_len;
  }
  queued_event->owned_data_len = data_len;
  if (queued_event->channel != NULL) {
    atomic_fetch_add_explicit(&queued_event->channel->event_refs, 1u,
                              memory_order_relaxed);
  }
  atomic_fetch_add_explicit(&peer->network_event_count, 1u,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&peer->refs, 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&peer->network_event_bytes, data_len,
                            memory_order_relaxed);
  h2_pal_result_t result = H2_PAL_OK;
  if (peer->network_events != NULL) {
    result = (h2_pal_result_t)h2_pal_queue_send(
        peer->owner->config.queue, peer->network_events, &queued_event,
        H2_PAL_QUEUE_NO_WAIT);
  } else {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  if (result == H2_PAL_OK) {
    return H2_PAL_OK;
  }
  atomic_fetch_sub_explicit(&peer->network_event_bytes, data_len,
                            memory_order_relaxed);
  atomic_fetch_sub_explicit(&peer->network_event_count, 1u,
                            memory_order_relaxed);
  atomic_fetch_sub_explicit(&peer->refs, 1u, memory_order_relaxed);
  if (queued_event->channel != NULL) {
    atomic_fetch_sub_explicit(&queued_event->channel->event_refs, 1u,
                              memory_order_relaxed);
  }
  h2_peer_free(peer->owner, queued_event);
  // PAL queues differ in their nonblocking-full result. Normalize here so
  // reliable channel delivery can retain its message and retry on WOULD_BLOCK.
  return result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK
             ? H2_PAL_ERR_FULL
             : result;
}

static void h2_peer_record_network_event_error(h2_pal_webrtc_peer_t *peer,
                                               h2_pal_result_t result) {
  if (result == H2_PAL_OK) {
    return;
  }
  // This caller cannot retain/retry the event. A full notification queue is
  // terminal overflow, not an ordinary poll timeout or temporary send busy.
  if (result == H2_PAL_ERR_FULL)
    result = H2_PAL_ERR_NO_SPACE;
  int expected = H2_PAL_OK;
  if (atomic_compare_exchange_strong_explicit(
          &peer->network_transport_result, &expected, result,
          memory_order_release, memory_order_relaxed)) {
    h2_peer_network_event_t event = {
        .type = H2_PEER_NETWORK_EVENT_ERROR,
        .error = result,
    };
    // Wake a waiting poll. If allocation/queue capacity prevents notification,
    // poll also observes the sticky error after draining the accepted prefix.
    (void)h2_peer_queue_network_event(peer, &event, NULL, 0u);
  }
}

void h2_peer_webrtc_emit_peer_state(h2_pal_webrtc_peer_t *peer,
                                    h2_pal_webrtc_peer_state_t state) {
  if (peer->state == state) {
    return;
  }
  peer->state = state;
  h2_peer_network_event_t event = {
      .type = H2_PEER_NETWORK_EVENT_PEER_STATE,
      .peer_state = state,
  };
  h2_peer_record_network_event_error(
      peer, h2_peer_queue_network_event(peer, &event, NULL, 0u));
}

void h2_peer_webrtc_emit_channel_state(h2_pal_webrtc_channel_t *channel,
                                       h2_pal_webrtc_channel_state_t state) {
  h2_pal_webrtc_peer_t *peer = channel->owner;
  h2_peer_network_event_t event = {
      .type = H2_PEER_NETWORK_EVENT_CHANNEL_STATE,
      .channel = channel,
      .channel_state = state,
  };
  h2_peer_record_network_event_error(
      peer, h2_peer_queue_network_event(peer, &event, NULL, 0u));
}

void h2_peer_webrtc_emit_local_sdp(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_sdp_type_t type,
                                   h2_pal_webrtc_str_t sdp) {
  h2_peer_network_event_t event = {
      .type = H2_PEER_NETWORK_EVENT_LOCAL_SDP,
      .sdp_type = type,
  };
  h2_peer_record_network_event_error(
      peer, h2_peer_queue_network_event(peer, &event, (const uint8_t *)sdp.data,
                                        sdp.len));
}

h2_pal_result_t h2_peer_webrtc_emit_channel_message(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel,
    const uint8_t *data, size_t len, int is_text) {
  h2_peer_network_event_t event = {
      .type = H2_PEER_NETWORK_EVENT_CHANNEL_MESSAGE,
      .channel = channel,
      .is_text = is_text != 0,
  };
  h2_pal_result_t result = h2_peer_queue_network_event(peer, &event, data, len);
  return result == H2_PAL_ERR_FULL ? H2_PAL_ERR_WOULD_BLOCK : result;
}

struct h2_peer_media_frame {
  h2_peer_media_frame_t *next;
  size_t len;
  uint8_t data[];
};

void h2_peer_webrtc_discard_media(h2_pal_webrtc_peer_t *peer) {
  while (peer->media_receive_head != NULL) {
    h2_peer_media_frame_t *frame = peer->media_receive_head;
    peer->media_receive_head = frame->next;
    h2_peer_free(peer->owner, frame);
  }
  peer->media_receive_tail = NULL;
  peer->media_receive_count = 0u;
  peer->media_pending_opus_len = 0u;
}

static h2_pal_result_t h2_peer_media_receive_enqueue(h2_pal_webrtc_peer_t *peer,
                                                     const uint8_t *opus,
                                                     size_t len) {
  /* RTP cannot apply backpressure to its sender. Keep latency bounded by
   * replacing the oldest buffered frame when playback falls behind; reliable
   * DataChannel delivery uses a separate queue and still retries. */
  if (peer->media_receive_count >= H2_PEER_MEDIA_RECEIVE_LIMIT) {
    h2_peer_media_frame_t *dropped = peer->media_receive_head;
    peer->media_receive_head = dropped->next;
    if (peer->media_receive_head == NULL)
      peer->media_receive_tail = NULL;
    --peer->media_receive_count;
    h2_peer_free(peer->owner, dropped);
  }
  h2_peer_media_frame_t *frame =
      h2_peer_alloc(peer->owner, sizeof(*frame) + len);
  if (frame == NULL) {
    char message[160];
    (void)snprintf(message, sizeof(message),
                   "event=drop reason=no_memory queue_depth=%zu"
                   " queue_capacity=%u",
                   peer->media_receive_count,
                   (unsigned int)H2_PEER_MEDIA_RECEIVE_LIMIT);
    h2_peer_media_log(peer, H2_PAL_LOG_WARN, "h2peer/media-rx", message);
    return H2_PAL_ERR_NO_MEMORY;
  }
  frame->next = NULL;
  frame->len = len;
  if (len != 0u)
    memcpy(frame->data, opus, len);
  if (peer->media_receive_tail != NULL)
    peer->media_receive_tail->next = frame;
  else
    peer->media_receive_head = frame;
  peer->media_receive_tail = frame;
  ++peer->media_receive_count;
  return H2_PAL_OK;
}

void h2_peer_webrtc_emit_opus_frame(h2_pal_webrtc_peer_t *peer,
                                    const uint8_t *opus, size_t opus_len) {
  if (peer->closed || atomic_load_explicit(&peer->network_transport_result,
                                           memory_order_acquire) != H2_PAL_OK)
    return;
  if ((opus == NULL && opus_len != 0u) ||
      opus_len > H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE) {
    h2_peer_record_network_event_error(peer, H2_PAL_ERR_FORMAT);
    return;
  }
  if (peer->media_track != NULL && peer->media_track->vtable != NULL &&
      peer->media_track->vtable->write != NULL) {
    h2_pal_result_t result = H2_PAL_ERR_WOULD_BLOCK;
    const bool attempted_write = peer->media_receive_head == NULL;
    if (attempted_write)
      result = peer->media_track->vtable->write(peer->media_track->user, opus,
                                                opus_len);
    // RTP has no consumer backpressure. Own a bounded FIFO while the Track is
    // busy; never replace the head or let a later packet bypass it.
    if (result == H2_PAL_ERR_WOULD_BLOCK)
      result = h2_peer_media_receive_enqueue(peer, opus, opus_len);
    h2_peer_record_network_event_error(peer, result);
    return;
  }
  h2_peer_network_event_t event = {
      .type = H2_PEER_NETWORK_EVENT_OPUS_FRAME,
  };
  h2_peer_record_network_event_error(
      peer, h2_peer_queue_network_event(peer, &event, opus, opus_len));
}

static h2_pal_webrtc_channel_t *h2_peer_find_channel(h2_pal_webrtc_peer_t *peer,
                                                     uint16_t stream_id) {
  for (h2_pal_webrtc_channel_t *channel = peer->channels; channel != NULL;
       channel = channel->next) {
    if (channel->info.stream_id == stream_id) {
      return channel;
    }
  }
  return NULL;
}

static int h2_peer_channel_is_current(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_webrtc_channel_t *channel,
                                      uint16_t stream_id, uint32_t generation) {
  h2_pal_webrtc_channel_t *current = h2_peer_find_channel(peer, stream_id);
  return current == channel && current->generation == generation;
}

static void h2_peer_free_channel(h2_pal_webrtc_peer_t *peer,
                                 h2_pal_webrtc_channel_t *channel);

static h2_pal_result_t
h2_peer_open_pending_channels(h2_pal_webrtc_peer_t *peer);

static int h2_peer_has_active_stream_reset(const h2_pal_webrtc_peer_t *peer) {
  for (size_t i = 0u; i < H2_PEER_STREAM_COUNT; ++i) {
    if (peer->stream_resets[i].active) {
      return 1;
    }
  }
  return 0;
}

static int h2_peer_stream_slot(const h2_pal_webrtc_peer_t *peer,
                               uint16_t stream_id, size_t *out_slot) {
  if (stream_id < peer->local_stream_first) {
    return 0;
  }
  uint16_t offset = (uint16_t)(stream_id - peer->local_stream_first);
  size_t slot = (size_t)(offset / 2u);
  if ((offset & 1u) != 0u || slot >= H2_PEER_LOCAL_STREAM_COUNT) {
    return 0;
  }
  *out_slot = slot;
  return 1;
}

static uint16_t h2_peer_stream_id_for_slot(const h2_pal_webrtc_peer_t *peer,
                                           size_t slot) {
  return (uint16_t)(peer->local_stream_first + (uint16_t)(slot * 2u));
}

static void h2_peer_unlink_channel(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_channel_t *channel) {
  h2_pal_webrtc_channel_t **cursor = &peer->channels;
  while (*cursor != NULL && *cursor != channel) {
    cursor = &(*cursor)->next;
  }
  if (*cursor == channel) {
    *cursor = channel->next;
  }
  channel->next = NULL;
}

static void h2_peer_terminal_channel(h2_pal_webrtc_channel_t *channel,
                                     h2_pal_webrtc_channel_state_t state) {
  if (channel == NULL || channel->terminal) {
    return;
  }
  h2_pal_webrtc_peer_t *peer = channel->owner;
  channel->terminal = 1;
  channel->open = 0;
  // Retire the slot before it can be reused. Event leases may delay freeing
  // this channel, but their release must never clear a new owner's ready bit.
  h2_peer_channel_ready_clear(channel);
  h2_peer_unlink_channel(peer, channel);
  h2_peer_webrtc_emit_channel_state(channel, state);
  unsigned int previous = atomic_fetch_or_explicit(
      &channel->event_refs, H2_PEER_CHANNEL_FREE_PENDING, memory_order_acq_rel);
  if (previous == 0u) {
    h2_peer_free_channel(peer, channel);
  }
}

static void h2_peer_terminal_all_channels(h2_pal_webrtc_peer_t *peer,
                                          h2_pal_webrtc_channel_state_t state) {
  while (peer->channels != NULL) {
    h2_peer_terminal_channel(peer->channels, state);
  }
}

static h2_pal_result_t h2_peer_submit_stream_reset(h2_pal_webrtc_peer_t *peer,
                                                   uint16_t stream_id) {
  return h2_peer_portable_reset_stream(peer, stream_id);
}

static h2_pal_result_t h2_peer_forget_stream(h2_pal_webrtc_peer_t *peer,
                                             uint16_t stream_id) {
  return h2_peer_portable_forget_stream(peer, stream_id);
}

static h2_pal_result_t h2_peer_fail_stream_resets(h2_pal_webrtc_peer_t *peer,
                                                  h2_pal_result_t result) {
  if (result == H2_PAL_OK) {
    result = H2_PAL_ERR_IO;
  }
  peer->stream_reset_failure = result;
  char message[64];
  (void)snprintf(message, sizeof(message), "stream reset failed rc=%d", result);
  (void)h2_pal_log_write(peer->owner->config.log, H2_PAL_LOG_ERROR, "h2peer",
                         message);
  h2_peer_terminal_all_channels(peer, H2_PAL_WEBRTC_CHANNEL_ERROR);
  h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_FAILED);
  h2_peer_portable_peer_close(peer);
  return result;
}

static h2_pal_result_t
h2_peer_service_stream_resets(h2_pal_webrtc_peer_t *peer) {
  if (peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  if (peer->stream_reset_failure != H2_PAL_OK) {
    return peer->stream_reset_failure;
  }
  int reset_in_flight = 0;
  for (size_t i = 0u; i < H2_PEER_STREAM_COUNT; ++i) {
    h2_peer_stream_reset_t *reset = &peer->stream_resets[i];
    if (!reset->active) {
      continue;
    }
    if (reset->outgoing_completed && reset->incoming_reset) {
      h2_pal_result_t result = h2_peer_forget_stream(peer, (uint16_t)i);
      if (result != H2_PAL_OK) {
        return h2_peer_fail_stream_resets(peer, result);
      }
      uint32_t generation = reset->generation;
      memset(reset, 0, sizeof(*reset));
      reset->generation = generation;
      continue;
    }
    if (reset->outgoing_submitted && !reset->outgoing_completed) {
      reset_in_flight = 1;
    }
  }
  if (reset_in_flight) {
    return H2_PAL_OK;
  }
  for (size_t i = 0u; i < H2_PEER_STREAM_COUNT; ++i) {
    h2_peer_stream_reset_t *reset = &peer->stream_resets[i];
    if (!reset->active || reset->outgoing_submitted) {
      continue;
    }
    h2_pal_result_t result = h2_peer_submit_stream_reset(peer, (uint16_t)i);
    if (result == H2_PAL_OK) {
      reset->outgoing_submitted = 1;
      return H2_PAL_OK;
    }
    if (result == H2_PAL_ERR_BUSY || result == H2_PAL_ERR_WOULD_BLOCK) {
      return H2_PAL_OK;
    }
    return h2_peer_fail_stream_resets(peer, result);
  }
  if (!h2_peer_has_active_stream_reset(peer)) {
    return h2_peer_open_pending_channels(peer);
  }
  return H2_PAL_OK;
}

void h2_peer_webrtc_on_stream_reset(
    h2_pal_webrtc_peer_t *peer, const h2_pal_sctp_stream_reset_event_t *event) {
  if (peer == NULL || event == NULL || peer->closed) {
    return;
  }
  if (event->stream_id >= H2_PEER_STREAM_COUNT) {
    peer->stream_reset_failure = H2_PAL_ERR_INVALID_ARG;
    return;
  }
  if (event->result != H2_PAL_OK) {
    peer->stream_reset_failure = event->result;
    return;
  }
  h2_peer_stream_reset_t *reset = &peer->stream_resets[event->stream_id];
  h2_pal_webrtc_channel_t *channel =
      h2_peer_find_channel(peer, event->stream_id);
  if (!reset->active) {
    if (event->direction != H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET ||
        channel == NULL) {
      return;
    }
    reset->active = 1;
    reset->generation = channel->generation;
  }
  if (event->direction == H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED) {
    if (!reset->outgoing_submitted) {
      return;
    }
    reset->outgoing_completed = 1;
  } else if (event->direction == H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET) {
    reset->incoming_reset = 1;
    if (channel != NULL && channel->generation == reset->generation) {
      h2_peer_terminal_channel(channel, H2_PAL_WEBRTC_CHANNEL_CLOSED);
    }
  } else {
    peer->stream_reset_failure = H2_PAL_ERR_INVALID_ARG;
  }
}

void h2_peer_webrtc_on_sctp_closed(h2_pal_webrtc_peer_t *peer) {
  if (peer == NULL || peer->closed) {
    return;
  }
  h2_peer_terminal_all_channels(peer, H2_PAL_WEBRTC_CHANNEL_CLOSED);
}

static h2_pal_result_t
h2_peer_webrtc_peer_create(void *user, h2_pal_webrtc_peer_t **out_peer) {
  h2_peer_t *owner = (h2_peer_t *)user;
  *out_peer = NULL;
  if (owner == NULL || owner->destroying) {
    return H2_PAL_ERR_CLOSED;
  }
  h2_pal_webrtc_peer_t *peer =
      (h2_pal_webrtc_peer_t *)h2_peer_control_alloc(owner, sizeof(*peer));
  if (peer == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  atomic_init(&peer->closed, 0);
  atomic_init(&peer->refs, 1u);
  atomic_fetch_add_explicit(&owner->refs, 1u, memory_order_relaxed);
  atomic_init(&peer->network_event_count, 0u);
  atomic_init(&peer->network_event_bytes, 0u);
  atomic_init(&peer->network_transport_result, H2_PAL_OK);
  atomic_init(&peer->network_error_reported, 0);
  peer->owner = owner;
  atomic_init(&peer->state, H2_PAL_WEBRTC_PEER_NEW);
  peer->local_stream_first = 1u;
  peer->next_stream_id = peer->local_stream_first;
  peer->next = owner->peers;
  owner->peers = peer;
  *out_peer = peer;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_webrtc_add_ice_server(h2_pal_webrtc_peer_t *peer,
                              const h2_pal_webrtc_ice_server_t *server) {
  if (peer->closed || peer->offer_started) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_result_t result = h2_peer_ice_validate_server(server);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (peer->ice_server_count == H2_PEER_ICE_SERVER_MAX) {
    return H2_PAL_ERR_FULL;
  }
  h2_peer_ice_server_t copied = {0};
  copied.url = h2_peer_copy_string(peer->owner, server->url);
  if (copied.url == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  copied.username = h2_peer_copy_string(peer->owner, server->username);
  if (server->username.len != 0u && copied.username == NULL) {
    h2_peer_free(peer->owner, copied.url);
    return H2_PAL_ERR_NO_MEMORY;
  }
  copied.credential = h2_peer_copy_string(peer->owner, server->credential);
  if (server->credential.len != 0u && copied.credential == NULL) {
    h2_peer_free(peer->owner, copied.username);
    h2_peer_free(peer->owner, copied.url);
    return H2_PAL_ERR_NO_MEMORY;
  }
  peer->ice_servers[peer->ice_server_count++] = copied;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_open_pending_channels(h2_pal_webrtc_peer_t *peer) {
  for (;;) {
    h2_pal_webrtc_channel_t *channel = peer->channels;
    while (channel != NULL &&
           (channel->remote_created || channel->wire_opened)) {
      channel = channel->next;
    }
    if (channel == NULL) {
      return H2_PAL_OK;
    }
    const uint16_t stream_id = channel->info.stream_id;
    const uint32_t generation = channel->generation;
    if (!peer->production_sctp_open)
      return H2_PAL_OK;
    h2_pal_result_t result = h2_peer_portable_channel_open(channel);
    if (result != H2_PAL_OK) {
      return result;
    }
    if (peer->closed ||
        !h2_peer_channel_is_current(peer, channel, stream_id, generation)) {
      return H2_PAL_ERR_CLOSED;
    }
    channel->wire_opened = 1;
  }
}

h2_pal_result_t h2_peer_webrtc_on_remote_channel_open(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_str_t label, uint16_t stream_id,
    int ordered, int reliable) {
  if (peer == NULL || peer->closed || label.data == NULL || label.len == 0u ||
      label.len > H2_PEER_CHANNEL_LABEL_MAX ||
      stream_id >= H2_PEER_STREAM_COUNT ||
      (stream_id & 1u) == (peer->local_stream_first & 1u) ||
      h2_peer_find_channel(peer, stream_id) != NULL ||
      peer->stream_resets[stream_id].active) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_webrtc_channel_t *channel =
      (h2_pal_webrtc_channel_t *)h2_peer_control_alloc(peer->owner,
                                                       sizeof(*channel));
  if (channel == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  atomic_init(&channel->ready_slot, UINT8_MAX);
  atomic_init(&channel->event_refs, 0u);
  atomic_init(&channel->open, 0);
  atomic_init(&channel->terminal, 0);
  h2_peer_channel_tx_init(channel);
  channel->label = h2_peer_copy_string(peer->owner, label);
  if (channel->label == NULL) {
    h2_peer_control_free(peer->owner, channel);
    return H2_PAL_ERR_NO_MEMORY;
  }
  channel->owner = peer;
  h2_pal_result_t ready_result = h2_peer_channel_ready_slot_allocate(channel);
  if (ready_result != H2_PAL_OK) {
    h2_peer_free(peer->owner, channel->label);
    h2_peer_control_free(peer->owner, channel);
    return ready_result;
  }
  channel->info.label.data = channel->label;
  channel->info.label.len = label.len;
  channel->info.stream_id = stream_id;
  channel->info.has_stream_id = 1;
  channel->info.ordered = ordered != 0;
  channel->info.reliable = reliable != 0;
  uint32_t generation = peer->stream_resets[stream_id].generation + 1u;
  if (generation == 0u) {
    generation = 1u;
  }
  peer->stream_resets[stream_id].generation = generation;
  channel->generation = generation;
  channel->open = 1;
  channel->wire_opened = 1;
  channel->remote_created = 1;
  channel->next = peer->channels;
  peer->channels = channel;
  h2_peer_webrtc_emit_channel_state(channel, H2_PAL_WEBRTC_CHANNEL_OPEN);
  return H2_PAL_OK;
}

static h2_pal_result_t h2_peer_webrtc_start_offer(h2_pal_webrtc_peer_t *peer) {
  if (peer->closed || peer->offer_started ||
      peer->state != H2_PAL_WEBRTC_PEER_NEW)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_result_t result = h2_peer_portable_start_offer(peer);
  if (result != H2_PAL_OK)
    h2_peer_portable_peer_close(peer);
  return result;
}

static h2_pal_result_t
h2_peer_webrtc_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                              h2_pal_webrtc_sdp_type_t type,
                              h2_pal_webrtc_str_t sdp) {
  if (peer->closed || !peer->offer_started || peer->remote_answer_set ||
      type != H2_PAL_WEBRTC_SDP_ANSWER) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_peer_portable_set_remote_sdp(peer, type, sdp);
}

static h2_pal_result_t
h2_peer_webrtc_create_data_channel(h2_pal_webrtc_peer_t *peer,
                                   const h2_pal_webrtc_channel_config_t *config,
                                   h2_pal_webrtc_channel_t **out_channel) {
  *out_channel = NULL;
  if (peer->closed || peer->state == H2_PAL_WEBRTC_PEER_FAILED) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (config->label.len > H2_PEER_CHANNEL_LABEL_MAX) {
    return H2_PAL_ERR_NO_SPACE;
  }
  uint16_t stream_id = config->stream_id;
  size_t stream_slot = 0u;
  if (config->has_stream_id) {
    if (!h2_peer_stream_slot(peer, stream_id, &stream_slot) ||
        h2_peer_find_channel(peer, stream_id) != NULL ||
        peer->stream_resets[stream_id].active) {
      return H2_PAL_ERR_INVALID_ARG;
    }
  } else {
    size_t first_slot = 0u;
    if (!h2_peer_stream_slot(peer, peer->next_stream_id, &first_slot)) {
      first_slot = 0u;
    }
    int found = 0;
    for (size_t i = 0u; i < H2_PEER_LOCAL_STREAM_COUNT; ++i) {
      size_t candidate_slot = (first_slot + i) % H2_PEER_LOCAL_STREAM_COUNT;
      uint16_t candidate = h2_peer_stream_id_for_slot(peer, candidate_slot);
      if (h2_peer_find_channel(peer, candidate) == NULL &&
          !peer->stream_resets[candidate].active) {
        stream_id = candidate;
        stream_slot = candidate_slot;
        found = 1;
        break;
      }
    }
    if (!found) {
      return H2_PAL_ERR_NO_SPACE;
    }
  }
  h2_pal_webrtc_channel_t *channel =
      (h2_pal_webrtc_channel_t *)h2_peer_control_alloc(peer->owner,
                                                       sizeof(*channel));
  if (channel == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  atomic_init(&channel->ready_slot, UINT8_MAX);
  atomic_init(&channel->event_refs, 0u);
  atomic_init(&channel->open, 0);
  atomic_init(&channel->terminal, 0);
  h2_peer_channel_tx_init(channel);
  channel->label = h2_peer_copy_string(peer->owner, config->label);
  if (channel->label == NULL) {
    h2_peer_control_free(peer->owner, channel);
    return H2_PAL_ERR_NO_MEMORY;
  }
  channel->owner = peer;
  h2_pal_result_t ready_result = h2_peer_channel_ready_slot_allocate(channel);
  if (ready_result != H2_PAL_OK) {
    h2_peer_free(peer->owner, channel->label);
    h2_peer_control_free(peer->owner, channel);
    return ready_result;
  }
  channel->info.label.data = channel->label;
  channel->info.label.len = config->label.len;
  channel->info.stream_id = stream_id;
  channel->info.has_stream_id = 1;
  channel->info.ordered = config->ordered != 0;
  channel->info.reliable = config->reliable != 0;
  uint32_t generation = peer->stream_resets[stream_id].generation + 1u;
  if (generation == 0u) {
    generation = 1u;
  }
  peer->stream_resets[stream_id].generation = generation;
  channel->generation = generation;
  channel->next = peer->channels;
  peer->channels = channel;
  if (!config->has_stream_id) {
    size_t next_slot = (stream_slot + 1u) % H2_PEER_LOCAL_STREAM_COUNT;
    peer->next_stream_id = h2_peer_stream_id_for_slot(peer, next_slot);
  }
  h2_pal_result_t result = H2_PAL_OK;
  const uint32_t channel_generation = channel->generation;
  int attempted_open = 0;
  if (peer->production_sctp_open) {
    attempted_open = 1;
    result = h2_peer_portable_channel_open(channel);
  }
  const int channel_current =
      h2_peer_channel_is_current(peer, channel, stream_id, channel_generation);
  if (result != H2_PAL_OK) {
    if (channel_current) {
      h2_peer_unlink_channel(peer, channel);
      h2_peer_free_channel(peer, channel);
    }
    return result;
  }
  if (peer->closed || !channel_current) {
    return H2_PAL_ERR_CLOSED;
  }
  if (attempted_open) {
    channel->wire_opened = 1;
  }
  *out_channel = channel;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_peer_webrtc_set_track(h2_pal_webrtc_peer_t *peer,
                                                h2_pal_webrtc_track_t *track) {
  if (peer == NULL || peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  if (peer->offer_started || track == NULL || peer->media_track != NULL ||
      (track->vtable == NULL && track->native_handle == NULL)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  peer->media_track = track;
  peer->media_pending_opus_len = 0u;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_webrtc_unset_track(h2_pal_webrtc_peer_t *peer,
                           h2_pal_webrtc_track_t *track) {
  if (peer == NULL || peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (track == NULL || peer->media_track != track)
    return H2_PAL_ERR_INVALID_STATE;
  peer->media_track = NULL;
  h2_peer_webrtc_discard_media(peer);
  return H2_PAL_OK;
}

/* Called only by the network task, never by public peer_poll. */
static h2_pal_result_t h2_peer_webrtc_poll(h2_pal_webrtc_peer_t *peer,
                                           int timeout_ms) {
  if (peer->closed)
    return H2_PAL_ERR_CLOSED;
  if (!peer->offer_started || !peer->remote_answer_set || timeout_ms < 0)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_result_t result = h2_peer_service_stream_resets(peer);
  if (result == H2_PAL_OK)
    result = h2_peer_portable_poll(peer, timeout_ms);
  h2_pal_result_t reset_result = h2_peer_service_stream_resets(peer);
  if (reset_result != H2_PAL_OK)
    result = reset_result;
  return result;
}

static h2_pal_result_t h2_peer_webrtc_send_opus(h2_pal_webrtc_peer_t *peer,
                                                const uint8_t *opus,
                                                size_t opus_len) {
  if (peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  return h2_peer_portable_send_opus(peer, opus, opus_len);
}

static h2_pal_result_t
h2_peer_webrtc_channel_send(h2_pal_webrtc_channel_t *channel,
                            const uint8_t *data, size_t len, int is_text) {
  h2_pal_webrtc_peer_t *peer = channel->owner;
  if (peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  if (!channel->open || peer->state != H2_PAL_WEBRTC_PEER_CONNECTED) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_peer_portable_channel_send(channel, data, len, is_text);
}

static void h2_peer_webrtc_channel_close(h2_pal_webrtc_channel_t *channel) {
  if (channel == NULL || channel->terminal) {
    return;
  }
  h2_pal_webrtc_peer_t *peer = channel->owner;
  if (channel->wire_opened) {
    uint16_t stream_id = channel->info.stream_id;
    if (stream_id >= H2_PEER_STREAM_COUNT) {
      peer->stream_reset_failure = H2_PAL_ERR_INVALID_ARG;
    } else {
      h2_peer_stream_reset_t *reset = &peer->stream_resets[stream_id];
      reset->active = 1;
      reset->generation = channel->generation;
    }
  }
  h2_peer_terminal_channel(channel, H2_PAL_WEBRTC_CHANNEL_CLOSED);
  h2_peer_record_network_event_error(peer, h2_peer_service_stream_resets(peer));
}

static void h2_peer_free_channel(h2_pal_webrtc_peer_t *peer,
                                 h2_pal_webrtc_channel_t *channel) {
  h2_peer_channel_ready_clear(channel);
  for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i) {
    atomic_store_explicit(&channel->tx_state[i], 0u, memory_order_release);
    h2_peer_free_tx_item(peer->owner, channel->tx_storage[i]);
    channel->tx_storage[i] = NULL;
  }
  atomic_store_explicit(&channel->tx_head, 0u, memory_order_release);
  atomic_store_explicit(&channel->tx_tail, 0u, memory_order_release);
  h2_peer_free(peer->owner, channel->label);
  h2_peer_control_free(peer->owner, channel);
}

static void h2_peer_free_ice_servers(h2_pal_webrtc_peer_t *peer) {
  for (size_t i = 0u; i < peer->ice_server_count; ++i) {
    h2_peer_free(peer->owner, peer->ice_servers[i].credential);
    h2_peer_free(peer->owner, peer->ice_servers[i].username);
    h2_peer_free(peer->owner, peer->ice_servers[i].url);
  }
  peer->ice_server_count = 0u;
}

static void h2_peer_webrtc_peer_close(h2_pal_webrtc_peer_t *peer) {
  if (peer->closed)
    return;
  peer->closed = 1;
  h2_peer_portable_peer_close(peer);
  peer->media_track = NULL;
  h2_peer_webrtc_discard_media(peer);
  h2_peer_terminal_all_channels(peer, H2_PAL_WEBRTC_CHANNEL_CLOSED);
  h2_peer_free_ice_servers(peer);
  h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CLOSED);
}

static void h2_peer_network_release_event(h2_peer_network_event_t *event) {
  h2_pal_webrtc_peer_t *peer = event->peer;
  h2_peer_t *owner = peer->owner;
  h2_pal_webrtc_channel_t *channel = event->channel;
  atomic_fetch_sub_explicit(&peer->network_event_bytes, event->owned_data_len,
                            memory_order_relaxed);
  atomic_fetch_sub_explicit(&peer->network_event_count, 1u,
                            memory_order_acq_rel);
  if (channel != NULL) {
    unsigned int previous = atomic_fetch_sub_explicit(&channel->event_refs, 1u,
                                                      memory_order_acq_rel);
    if (previous == (H2_PEER_CHANNEL_FREE_PENDING | 1u)) {
      h2_peer_free_channel(peer, channel);
    }
  }
  h2_peer_free(owner, event);
  h2_peer_connection_release(peer);
}

static void h2_peer_public_event_release(h2_pal_webrtc_event_t *event) {
  h2_peer_network_event_t *owned =
      event == NULL ? NULL : (h2_peer_network_event_t *)event->_private;
  if (owned != NULL)
    h2_peer_network_release_event(owned);
  if (event != NULL)
    *event = (h2_pal_webrtc_event_t){0};
}

static h2_pal_result_t
h2_peer_network_export_event(h2_peer_network_event_t *event,
                             h2_pal_webrtc_event_t *out_event) {
  h2_pal_webrtc_event_kind_t kind;
  switch (event->type) {
  case H2_PEER_NETWORK_EVENT_PEER_STATE:
    kind = H2_PAL_WEBRTC_EVENT_PEER_STATE;
    break;
  case H2_PEER_NETWORK_EVENT_LOCAL_SDP:
    kind = H2_PAL_WEBRTC_EVENT_LOCAL_SDP;
    break;
  case H2_PEER_NETWORK_EVENT_CHANNEL_STATE:
    kind = H2_PAL_WEBRTC_EVENT_CHANNEL_STATE;
    break;
  case H2_PEER_NETWORK_EVENT_CHANNEL_MESSAGE:
    kind = H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE;
    break;
  case H2_PEER_NETWORK_EVENT_OPUS_FRAME:
    kind = H2_PAL_WEBRTC_EVENT_OPUS_FRAME;
    break;
  case H2_PEER_NETWORK_EVENT_SEND_READY:
    kind = H2_PAL_WEBRTC_EVENT_WRITABLE;
    atomic_store_explicit(&event->peer->network_send_wakeup_queued, 0,
                          memory_order_release);
    break;
  case H2_PEER_NETWORK_EVENT_ERROR:
    if (atomic_exchange_explicit(&event->peer->network_error_reported, 1,
                                 memory_order_acq_rel)) {
      h2_peer_network_release_event(event);
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    kind = H2_PAL_WEBRTC_EVENT_ERROR;
    break;
  default:
    h2_peer_network_release_event(event);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *out_event = (h2_pal_webrtc_event_t){
      .kind = kind,
      .peer = event->peer,
      .channel = event->channel,
      .peer_state = event->peer_state,
      .channel_state = event->channel_state,
      .sdp_type = event->sdp_type,
      .sdp = {.data = (const char *)event->data, .len = event->data_len},
      .data = event->data,
      .data_len = event->data_len,
      .is_text = event->is_text,
      .error = event->error,
      ._private = event,
      ._release = h2_peer_public_event_release,
  };
  if (event->channel != NULL)
    out_event->channel_info = event->channel->info;
  return H2_PAL_OK;
}

static void h2_peer_network_discard_available(h2_pal_webrtc_peer_t *peer) {
  if (peer->network_events == NULL)
    return;
  for (;;) {
    h2_peer_network_event_t *event = NULL;
    h2_pal_result_t result = (h2_pal_result_t)h2_pal_queue_recv(
        peer->owner->config.queue, peer->network_events, &event,
        H2_PAL_QUEUE_NO_WAIT);
    if (result != H2_PAL_OK) {
      break;
    }
    h2_peer_network_release_event(event);
  }
}

static void h2_peer_network_destroy_resources(h2_pal_webrtc_peer_t *peer) {
  h2_peer_t *owner = peer->owner;
  peer->media_track = NULL;
  h2_peer_webrtc_discard_media(peer);
  (void)atomic_exchange_explicit(&peer->rtp_pending, NULL,
                                 memory_order_acq_rel);
  h2_peer_free_tx_item(owner, peer->rtp_storage);
  peer->rtp_storage = NULL;
  h2_pal_queue_destroy(owner->config.queue, peer->network_events);
  h2_pal_queue_destroy(owner->config.queue, peer->network_responses);
  h2_pal_queue_destroy(owner->config.queue, peer->network_commands);
  (void)h2_pal_mutex_destroy(owner->config.sync, peer->network_request_mutex);
  peer->network_events = NULL;
  peer->network_responses = NULL;
  peer->network_commands = NULL;
  peer->network_request_mutex = NULL;
  peer->network_task = NULL;
}

static h2_pal_result_t
h2_peer_network_process_command(h2_pal_webrtc_peer_t *peer,
                                const h2_peer_network_command_t *command,
                                h2_peer_network_response_t *response) {
  response->result = H2_PAL_OK;
  response->channel = NULL;
  switch (command->type) {
  case H2_PEER_NETWORK_ADD_ICE_SERVER:
    response->result =
        h2_peer_webrtc_add_ice_server(peer, command->value.ice_server);
    break;
  case H2_PEER_NETWORK_START_OFFER:
    response->result = h2_peer_webrtc_start_offer(peer);
    break;
  case H2_PEER_NETWORK_SET_REMOTE_SDP:
    response->result = h2_peer_webrtc_set_remote_sdp(
        peer, command->value.remote_sdp.type, command->value.remote_sdp.sdp);
    break;
  case H2_PEER_NETWORK_CREATE_DATA_CHANNEL:
    response->result = h2_peer_webrtc_create_data_channel(
        peer, command->value.channel_config, &response->channel);
    break;
  case H2_PEER_NETWORK_SET_MEDIA_TRACK:
    response->result =
        h2_peer_webrtc_set_track(peer, command->value.media_track);
    break;
  case H2_PEER_NETWORK_UNSET_MEDIA_TRACK:
    response->result =
        h2_peer_webrtc_unset_track(peer, command->value.media_track);
    break;
  case H2_PEER_NETWORK_CHANNEL_CLOSE:
    h2_peer_webrtc_channel_close(command->value.channel);
    break;
  case H2_PEER_NETWORK_PEER_CLOSE:
    atomic_store_explicit(&peer->network_stop, 1, memory_order_release);
    h2_peer_webrtc_peer_close(peer);
    break;
  default:
    response->result = H2_PAL_ERR_INVALID_ARG;
    break;
  }
  h2_pal_result_t event_result = (h2_pal_result_t)atomic_load_explicit(
      &peer->network_transport_result, memory_order_acquire);
  if (command->type != H2_PEER_NETWORK_CHANNEL_CLOSE &&
      command->type != H2_PEER_NETWORK_PEER_CLOSE &&
      command->type != H2_PEER_NETWORK_UNSET_MEDIA_TRACK &&
      response->result == H2_PAL_OK && event_result != H2_PAL_OK) {
    response->result = event_result;
  }
  return response->result;
}

static int h2_peer_network_pump_transport(h2_pal_webrtc_peer_t *peer,
                                          int timeout_ms,
                                          int *transport_terminal,
                                          int *out_waited) {
  *out_waited = 0;
  if (*transport_terminal || !peer->offer_started || !peer->remote_answer_set ||
      peer->closed) {
    return 0;
  }
  if (atomic_load_explicit(&peer->network_transport_result,
                           memory_order_acquire) != H2_PAL_OK) {
    *transport_terminal = 1;
    return 0;
  }
  const unsigned int event_count =
      atomic_load_explicit(&peer->network_event_count, memory_order_relaxed);
  const size_t event_bytes =
      atomic_load_explicit(&peer->network_event_bytes, memory_order_relaxed);
  if (event_count >= H2_PEER_NETWORK_EVENT_HIGH_WATER ||
      event_bytes >= H2_PEER_NETWORK_EVENT_BYTES_HIGH_WATER) {
    return 0;
  }
  h2_pal_result_t result = H2_PAL_OK;
  int made_progress = 0;
  if (timeout_ms > 0) {
    *out_waited = 1;
  }
  if (h2_peer_portable_async_receive_supported(peer)) {
    h2_pal_net_addr_t addr;
    uint8_t packet[H2_PEER_WIRE_PACKET_MAX];
    int received = h2_peer_portable_receive_datagram(
        peer, &addr, packet, sizeof(packet), (uint32_t)timeout_ms);
    if (received > 0) {
      result = h2_peer_service_stream_resets(peer);
      if (result == H2_PAL_OK) {
        result = h2_peer_portable_service_datagram(peer, &addr, packet,
                                                   (size_t)received);
      }
      h2_pal_result_t reset_result = h2_peer_service_stream_resets(peer);
      if (reset_result != H2_PAL_OK) {
        result = reset_result;
      }
      made_progress = 1;
    } else if (received == 0 || received == H2_PAL_ERR_TIMEOUT ||
               received == H2_PAL_ERR_WOULD_BLOCK) {
      result = h2_peer_service_stream_resets(peer);
      if (result == H2_PAL_OK) {
        result = h2_peer_portable_service_datagram(peer, NULL, NULL, 0u);
      }
    } else {
      result = H2_PAL_ERR_IO;
    }
  } else {
    result = h2_peer_webrtc_poll(peer, timeout_ms);
  }
  if (result != H2_PAL_OK && result != H2_PAL_ERR_TIMEOUT &&
      result != H2_PAL_ERR_WOULD_BLOCK) {
    h2_peer_record_network_event_error(peer, result);
    *transport_terminal = 1;
  }
  return made_progress;
}

static void
h2_peer_network_send_response(h2_pal_webrtc_peer_t *peer,
                              const h2_peer_network_response_t *response) {
  (void)h2_pal_queue_send(peer->owner->config.queue, peer->network_responses,
                          response, H2_PAL_QUEUE_WAIT_FOREVER);
}

static int h2_peer_network_service_rtp(h2_pal_webrtc_peer_t *peer) {
  if (atomic_load_explicit(&peer->network_transport_result,
                           memory_order_acquire) != H2_PAL_OK)
    return 0;
  h2_peer_tx_item_t *item =
      atomic_load_explicit(&peer->rtp_pending, memory_order_acquire);
  if (item == NULL || item == H2_PEER_TX_RESERVED) {
    return 0;
  }

  h2_pal_result_t result =
      h2_peer_webrtc_send_opus(peer, item->data, item->len);
  if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
    return 0;
  }
  (void)atomic_exchange_explicit(&peer->rtp_pending, NULL,
                                 memory_order_acq_rel);
  h2_peer_network_notify_send_ready(peer);

  if (result != H2_PAL_OK) {
    h2_peer_record_network_event_error(peer, result);
  }
  return 1;
}

static h2_pal_webrtc_channel_t *
h2_peer_find_channel_by_ready_slot(h2_pal_webrtc_peer_t *peer,
                                   uint8_t ready_slot) {
  for (h2_pal_webrtc_channel_t *channel = peer->channels; channel != NULL;
       channel = channel->next) {
    if (channel->ready_slot == ready_slot) {
      return channel;
    }
  }
  return NULL;
}

/* Sends queued channel messages for one network round. Channels take turns
 * in ready-slot order, one message per turn, until the round budget
 * (H2_PEER_NETWORK_CHANNEL_ROUND_BYTES / _MESSAGES) is spent, SCTP pushes
 * back, or nothing is left. Ready bits stay set in *snapshot for channels
 * that still hold messages so the next round resumes with them. */
int h2_peer_network_service_channel(h2_pal_webrtc_peer_t *peer,
                                    uint32_t *snapshot) {
  int made_progress = 0;
  size_t round_bytes = 0u;
  size_t round_messages = 0u;
  const uint8_t start_slot = peer->channel_round_robin;
  for (;;) {
    int sent_this_pass = 0;
    for (size_t offset = 0u; offset < H2_PEER_READY_CHANNEL_COUNT; ++offset) {
      if (atomic_load_explicit(&peer->network_transport_result,
                               memory_order_acquire) != H2_PAL_OK)
        return made_progress;
      const uint8_t ready_slot =
          (uint8_t)((start_slot + offset) % H2_PEER_READY_CHANNEL_COUNT);
      const uint32_t bit = UINT32_C(1) << ready_slot;
      if ((*snapshot & bit) == 0u) {
        continue;
      }
      h2_pal_webrtc_channel_t *channel =
          h2_peer_find_channel_by_ready_slot(peer, ready_slot);
      if (channel == NULL) {
        *snapshot &= ~bit;
        continue;
      }
      const uint8_t head =
          atomic_load_explicit(&channel->tx_head, memory_order_acquire);
      if (atomic_load_explicit(&channel->tx_state[head],
                               memory_order_acquire) != 2u) {
        *snapshot &= ~bit;
        continue;
      }
      h2_peer_tx_item_t *item = channel->tx_storage[head];
      h2_pal_result_t result = h2_peer_webrtc_channel_send(
          channel, item->data, item->len, item->is_text);

      if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
        peer->channel_round_robin = ready_slot;
        return made_progress;
      }
      const size_t sent_len = item->len;
      const uint8_t next_head =
          (uint8_t)((head + 1u) % H2_PEER_INPUT_SLOT_COUNT);
      atomic_store_explicit(&channel->tx_head, next_head, memory_order_release);
      atomic_store_explicit(&channel->tx_state[head], 0u, memory_order_release);
      if (atomic_load_explicit(&channel->tx_state[next_head],
                               memory_order_acquire) != 2u) {
        *snapshot &= ~bit;
      }
      h2_peer_network_notify_send_ready(peer);
      peer->channel_round_robin =
          (uint8_t)((ready_slot + 1u) % H2_PEER_READY_CHANNEL_COUNT);
      if (result != H2_PAL_OK && result != H2_PAL_ERR_CLOSED &&
          result != H2_PAL_ERR_INVALID_STATE) {
        h2_peer_record_network_event_error(peer, result);
      }
      made_progress = 1;
      sent_this_pass = 1;
      round_bytes += sent_len;
      ++round_messages;
      if (round_bytes >= H2_PEER_NETWORK_CHANNEL_ROUND_BYTES ||
          round_messages >= H2_PEER_NETWORK_CHANNEL_ROUND_MESSAGES) {
        return made_progress;
      }
    }
    if (!sent_this_pass) {
      return made_progress;
    }
  }
}

static void h2_peer_network_task(void *context) {
  h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)context;
  int transport_terminal = 0;
  uint32_t channel_snapshot = 0u;
  while (!atomic_load_explicit(&peer->network_stop, memory_order_acquire)) {
    h2_peer_network_command_t command;
    h2_pal_result_t command_result = (h2_pal_result_t)h2_pal_queue_recv(
        peer->owner->config.queue, peer->network_commands, &command,
        H2_PAL_QUEUE_NO_WAIT);
    if (command_result == H2_PAL_OK) {
      h2_peer_network_response_t response;
      (void)h2_peer_network_process_command(peer, &command, &response);
      h2_peer_network_send_response(peer, &response);

      continue;
    }

    if (!transport_terminal) {
      const h2_pal_result_t media_result = h2_peer_webrtc_service_media(peer);
      if (media_result != H2_PAL_OK) {
        h2_peer_record_network_event_error(peer, media_result);
        transport_terminal = 1;
      }
    }

    int transport_progress = 0;
    for (size_t packet_count = 0u; packet_count < H2_PEER_NETWORK_UDP_BURST_MAX;
         ++packet_count) {
      int receive_waited = 0;
      const int receive_progress = h2_peer_network_pump_transport(
          peer, 0, &transport_terminal, &receive_waited);
      if (!receive_progress) {
        break;
      }
      transport_progress = 1;
    }

    // Merge newly readied channels every round so a channel still draining a
    // deep queue cannot hide another channel's first message.
    channel_snapshot |= atomic_exchange_explicit(&peer->channel_ready, 0u,
                                                 memory_order_acq_rel);
    h2_peer_tx_item_t *rtp_pending =
        atomic_load_explicit(&peer->rtp_pending, memory_order_acquire);
    const int has_channel_work = channel_snapshot != 0u;
    bool writable = false;
    if (has_channel_work) {
      h2_pal_result_t writable_result =
          h2_peer_portable_sctp_is_writable(peer, &writable);
      if (writable_result == H2_PAL_ERR_INVALID_STATE ||
          writable_result == H2_PAL_ERR_CLOSED) {
        writable = false;
      } else if (writable_result != H2_PAL_OK) {
        h2_peer_record_network_event_error(peer, writable_result);
        writable = false;
      }
    }

    const int rtp_progress =
        rtp_pending != NULL && rtp_pending != H2_PEER_TX_RESERVED
            ? h2_peer_network_service_rtp(peer)
            : 0;
    const int channel_progress =
        writable ? h2_peer_network_service_channel(peer, &channel_snapshot) : 0;
    if (rtp_progress || channel_progress) {
      continue;
    }

    if (transport_progress) {
      continue;
    }

    // Only wait after an idle round. A send frees a slot and wakes its
    // producer; waiting here would delay that producer's next frame even when
    // queued.
    int transport_waited = 0;
    if (h2_peer_network_pump_transport(peer, H2_PEER_NETWORK_IDLE_WAIT_MS,
                                       &transport_terminal,
                                       &transport_waited)) {
      continue;
    }
    if (!transport_waited) {
      (void)h2_pal_time_sleep_ms(peer->owner->config.time,
                                 H2_PEER_NETWORK_IDLE_WAIT_MS);
    }
  }
  atomic_store_explicit(&peer->network_stopped, 1, memory_order_release);
}

static h2_pal_result_t
h2_peer_network_call(h2_pal_webrtc_peer_t *peer,
                     const h2_peer_network_command_t *command,
                     h2_peer_network_response_t *out_response) {
  if (atomic_load_explicit(&peer->network_stopped, memory_order_acquire)) {
    return H2_PAL_ERR_CLOSED;
  }
  h2_pal_result_t result =
      h2_pal_mutex_lock(peer->owner->config.sync, peer->network_request_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (atomic_load_explicit(&peer->network_stop, memory_order_acquire) ||
      atomic_load_explicit(&peer->network_stopped, memory_order_acquire)) {
    (void)h2_pal_mutex_unlock(peer->owner->config.sync,
                              peer->network_request_mutex);
    return H2_PAL_ERR_CLOSED;
  }
  result = (h2_pal_result_t)h2_pal_queue_send(peer->owner->config.queue,
                                              peer->network_commands, command,
                                              H2_PAL_QUEUE_WAIT_FOREVER);
  h2_peer_network_response_t response = {
      .result = result,
  };
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_queue_recv(
        peer->owner->config.queue, peer->network_responses, &response,
        H2_PAL_QUEUE_WAIT_FOREVER);
  }
  h2_pal_result_t unlock_result = h2_pal_mutex_unlock(
      peer->owner->config.sync, peer->network_request_mutex);
  if (result == H2_PAL_OK && unlock_result != H2_PAL_OK) {
    result = unlock_result;
  }
  if (result == H2_PAL_OK) {
    result = response.result;
  }
  if (out_response != NULL) {
    *out_response = response;
  }
  return result;
}

static void h2_peer_network_unlink_created_peer(h2_pal_webrtc_peer_t *peer) {
  h2_pal_webrtc_peer_t **cursor = &peer->owner->peers;
  while (*cursor != NULL && *cursor != peer) {
    cursor = &(*cursor)->next;
  }
  if (*cursor == peer) {
    *cursor = peer->next;
  }
}

static h2_pal_result_t h2_peer_network_init(h2_pal_webrtc_peer_t *peer) {
  h2_peer_t *owner = peer->owner;
  const h2_pal_mutex_config_t mutex_config = {
      .name = "h2peer/net/request",
      .allocator = owner->config.mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  h2_pal_result_t result = h2_pal_mutex_create(
      owner->config.sync, &mutex_config, &peer->network_request_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  const h2_pal_queue_config_t command_config = {
      .name = "h2peer/net/commands",
      .item_size = sizeof(h2_peer_network_command_t),
      .item_count = H2_PEER_NETWORK_COMMAND_COUNT,
      .allocator = owner->config.mem,
  };
  result = (h2_pal_result_t)h2_pal_queue_create(
      owner->config.queue, &command_config, &peer->network_commands);
  if (result != H2_PAL_OK) {
    goto fail;
  }
  const h2_pal_queue_config_t response_config = {
      .name = "h2peer/net/responses",
      .item_size = sizeof(h2_peer_network_response_t),
      .item_count = 1u,
      .allocator = owner->config.mem,
  };
  result = (h2_pal_result_t)h2_pal_queue_create(
      owner->config.queue, &response_config, &peer->network_responses);
  if (result != H2_PAL_OK) {
    goto fail;
  }
  const h2_pal_queue_config_t event_config = {
      .name = "h2peer/net/events",
      .item_size = sizeof(h2_peer_network_event_t *),
      .item_count = H2_PEER_NETWORK_EVENT_COUNT,
      .allocator = owner->config.mem,
  };
  result = (h2_pal_result_t)h2_pal_queue_create(
      owner->config.queue, &event_config, &peer->network_events);
  if (result != H2_PAL_OK) {
    goto fail;
  }
  atomic_init(&peer->network_send_wakeup_queued, 0);
  atomic_init(&peer->network_stop, 0);
  atomic_init(&peer->network_stopped, 0);
  atomic_init(&peer->network_poll_active, 0);
  atomic_init(&peer->rtp_pending, NULL);
  atomic_init(&peer->channel_ready, 0u);
  const h2_pal_task_options_t task_options = {
      .name = h2_peer_network_task_name,
      .min_stack_size = H2_PEER_NETWORK_STACK_SIZE,
  };
  result = h2_pal_task_start(owner->config.task, &task_options,
                             h2_peer_network_task, peer, &peer->network_task);
  if (result == H2_PAL_OK) {
    return H2_PAL_OK;
  }

fail:
  h2_pal_queue_destroy(owner->config.queue, peer->network_events);
  h2_pal_queue_destroy(owner->config.queue, peer->network_responses);
  h2_pal_queue_destroy(owner->config.queue, peer->network_commands);
  (void)h2_pal_mutex_destroy(owner->config.sync, peer->network_request_mutex);
  peer->network_events = NULL;
  peer->network_responses = NULL;
  peer->network_commands = NULL;
  peer->network_request_mutex = NULL;
  return result;
}

static h2_pal_result_t
h2_peer_network_peer_create(void *user, h2_pal_webrtc_peer_t **out_peer) {
  h2_peer_t *owner = (h2_peer_t *)user;
  h2_pal_result_t result = h2_peer_webrtc_peer_create(user, out_peer);
  if (result != H2_PAL_OK) {
    return result;
  }
  result = h2_peer_network_init(*out_peer);
  if (result == H2_PAL_OK) {
    return H2_PAL_OK;
  }
  h2_pal_webrtc_peer_t *peer = *out_peer;
  *out_peer = NULL;
  h2_peer_network_unlink_created_peer(peer);
  h2_peer_control_free(owner, peer);
  h2_peer_owner_release(owner);
  return result;
}

static h2_pal_result_t
h2_peer_network_add_ice_server(h2_pal_webrtc_peer_t *peer,
                               const h2_pal_webrtc_ice_server_t *server) {
  const h2_peer_network_command_t command = {
      .type = H2_PEER_NETWORK_ADD_ICE_SERVER,
      .value.ice_server = server,
  };
  return h2_peer_network_call(peer, &command, NULL);
}

static h2_pal_result_t h2_peer_network_start_offer(h2_pal_webrtc_peer_t *peer) {
  const h2_peer_network_command_t command = {
      .type = H2_PEER_NETWORK_START_OFFER,
  };
  h2_pal_result_t result = h2_peer_network_call(peer, &command, NULL);
  return result;
}

static h2_pal_result_t
h2_peer_network_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                               h2_pal_webrtc_sdp_type_t type,
                               h2_pal_webrtc_str_t sdp) {
  const h2_peer_network_command_t command = {
      .type = H2_PEER_NETWORK_SET_REMOTE_SDP,
      .value.remote_sdp =
          {
              .type = type,
              .sdp = sdp,
          },
  };
  return h2_peer_network_call(peer, &command, NULL);
}

static h2_pal_result_t h2_peer_network_create_data_channel(
    h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_channel_config_t *config,
    h2_pal_webrtc_channel_t **out_channel) {
  *out_channel = NULL;
  const h2_peer_network_command_t command = {
      .type = H2_PEER_NETWORK_CREATE_DATA_CHANNEL,
      .value.channel_config = config,
  };
  h2_peer_network_response_t response;
  h2_pal_result_t result = h2_peer_network_call(peer, &command, &response);
  if (result == H2_PAL_OK) {
    *out_channel = response.channel;
  }
  return result;
}

static h2_pal_result_t h2_peer_network_set_track(h2_pal_webrtc_peer_t *peer,
                                                 h2_pal_webrtc_track_t *track) {
  const h2_peer_network_command_t command = {
      .type = H2_PEER_NETWORK_SET_MEDIA_TRACK,
      .value.media_track = track,
  };
  return h2_peer_network_call(peer, &command, NULL);
}

static h2_pal_result_t
h2_peer_network_unset_track(h2_pal_webrtc_peer_t *peer,
                            h2_pal_webrtc_track_t *track) {
  if (peer == NULL || track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_peer_network_command_t command = {
      .type = H2_PEER_NETWORK_UNSET_MEDIA_TRACK,
      .value.media_track = track,
  };
  return h2_peer_network_call(peer, &command, NULL);
}

/*
 * A waiting poll owns no lifecycle reference to the queue it receives from, so
 * it never blocks longer than one slice without re-observing the stop request
 * that a concurrent close publishes before joining the worker.
 */
#define H2_PEER_POLL_WAIT_SLICE_MS 20u

static h2_pal_result_t
h2_peer_network_poll_locked(h2_pal_webrtc_peer_t *peer, int timeout_ms,
                            h2_pal_webrtc_event_t *out_event) {
  uint64_t deadline_us = 0u;
  bool deadline_valid = false;
  uint32_t wait_ms = H2_PAL_QUEUE_NO_WAIT;
  for (;;) {
    h2_peer_network_event_t *event = NULL;
    h2_pal_result_t result = (h2_pal_result_t)h2_pal_queue_recv(
        peer->owner->config.queue, peer->network_events, &event, wait_ms);
    if (result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK) {
      const h2_pal_result_t failure = (h2_pal_result_t)atomic_load_explicit(
          &peer->network_transport_result, memory_order_acquire);
      if (failure != H2_PAL_OK) {
        if (atomic_exchange_explicit(&peer->network_error_reported, 1,
                                     memory_order_acq_rel))
          return failure;
        *out_event = (h2_pal_webrtc_event_t){
            .kind = H2_PAL_WEBRTC_EVENT_ERROR,
            .peer = peer,
            .error = failure,
        };
        return H2_PAL_OK;
      }
      if (timeout_ms == 0)
        return H2_PAL_ERR_WOULD_BLOCK;
      if (atomic_load_explicit(&peer->network_stop, memory_order_acquire))
        return H2_PAL_ERR_CLOSED;
      uint64_t now_us = 0u;
      const bool clock_ok = h2_pal_time_get_monotonic_us(
                                peer->owner->config.time, &now_us) == H2_PAL_OK;
      if (!clock_ok) {
        /* Without a usable clock the caller's deadline cannot be tracked
         * across slices; spend it on one bounded wait instead of spinning. */
        if (deadline_valid)
          return H2_PAL_ERR_TIMEOUT;
        deadline_valid = true;
        wait_ms = (uint32_t)timeout_ms;
        continue;
      }
      if (!deadline_valid) {
        deadline_us = now_us + (uint64_t)timeout_ms * 1000u;
        deadline_valid = true;
      } else if (now_us >= deadline_us) {
        return H2_PAL_ERR_TIMEOUT;
      }
      const uint64_t remaining_ms = (deadline_us - now_us) / 1000u;
      wait_ms = remaining_ms < H2_PEER_POLL_WAIT_SLICE_MS
                    ? (uint32_t)remaining_ms
                    : H2_PEER_POLL_WAIT_SLICE_MS;
      continue;
    }
    if (result != H2_PAL_OK)
      return result;
    result = h2_peer_network_export_event(event, out_event);
    if (result != H2_PAL_ERR_WOULD_BLOCK)
      return result;
    wait_ms = H2_PAL_QUEUE_NO_WAIT;
  }
}

static h2_pal_result_t h2_peer_network_poll(h2_pal_webrtc_peer_t *peer,
                                            int timeout_ms,
                                            h2_pal_webrtc_event_t *out_event) {
  if (timeout_ms < 0 || out_event == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_event = (h2_pal_webrtc_event_t){0};
  int idle = 0;
  if (!atomic_compare_exchange_strong_explicit(&peer->network_poll_active,
                                               &idle, 1, memory_order_acq_rel,
                                               memory_order_relaxed)) {
    return H2_PAL_ERR_BUSY;
  }
  const h2_pal_result_t result =
      h2_peer_network_poll_locked(peer, timeout_ms, out_event);
  atomic_store_explicit(&peer->network_poll_active, 0, memory_order_release);
  return result;
}

static h2_pal_result_t h2_peer_network_enqueue_opus(h2_pal_webrtc_peer_t *peer,
                                                    const uint8_t *opus,
                                                    size_t opus_len) {
  h2_peer_tx_item_t *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          &peer->rtp_pending, &expected, H2_PEER_TX_RESERVED,
          memory_order_acq_rel, memory_order_relaxed)) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_peer_tx_item_t *item = h2_peer_prepare_tx_item(
      peer->owner, &peer->rtp_storage, opus, opus_len, 0);
  if (item == NULL) {
    atomic_store_explicit(&peer->rtp_pending, NULL, memory_order_release);
    return H2_PAL_ERR_NO_MEMORY;
  }
  atomic_store_explicit(&peer->rtp_pending, item, memory_order_release);
  return H2_PAL_OK;
}

static h2_pal_result_t h2_peer_network_send_opus(h2_pal_webrtc_peer_t *peer,
                                                 const uint8_t *opus,
                                                 size_t opus_len) {
  const h2_pal_result_t failure = (h2_pal_result_t)atomic_load_explicit(
      &peer->network_transport_result, memory_order_acquire);
  if (failure != H2_PAL_OK)
    return failure;
  if (peer->closed || peer->state != H2_PAL_WEBRTC_PEER_CONNECTED) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_peer_network_enqueue_opus(peer, opus, opus_len);
}

h2_pal_result_t h2_peer_webrtc_service_media(h2_pal_webrtc_peer_t *peer) {
  h2_pal_webrtc_track_t *track = peer->media_track;
  if (track == NULL || track->vtable == NULL ||
      peer->state != H2_PAL_WEBRTC_PEER_CONNECTED || peer->closed) {
    return H2_PAL_OK;
  }
  const h2_pal_result_t failure = (h2_pal_result_t)atomic_load_explicit(
      &peer->network_transport_result, memory_order_acquire);
  if (failure != H2_PAL_OK)
    return failure;
  // Bound each round so draining the Track cannot starve network commands,
  // DataChannels or uplink. WOULD_BLOCK retains exactly the same head packet.
  for (size_t i = 0u; i < 8u && peer->media_receive_head != NULL; ++i) {
    h2_peer_media_frame_t *frame = peer->media_receive_head;
    h2_pal_result_t result = track->vtable->write(
        track->user, frame->len == 0u ? NULL : frame->data, frame->len);

    if (result == H2_PAL_ERR_WOULD_BLOCK)
      break;
    if (result != H2_PAL_OK) {
      h2_peer_webrtc_discard_media(peer);
      h2_peer_record_network_event_error(peer, result);
      return result;
    }
    peer->media_receive_head = frame->next;
    if (peer->media_receive_head == NULL)
      peer->media_receive_tail = NULL;
    --peer->media_receive_count;
    h2_peer_free(peer->owner, frame);
  }
  if (track->vtable->read == NULL)
    return H2_PAL_OK;
  if (peer->media_pending_opus_len == 0u) {
    size_t opus_len = 0u;
    h2_pal_result_t result =
        track->vtable->read(track->user, peer->media_pending_opus,
                            sizeof(peer->media_pending_opus), &opus_len);
    if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
      return H2_PAL_OK;
    }
    if (result != H2_PAL_OK)
      return result;
    if (opus_len == 0u || opus_len > sizeof(peer->media_pending_opus))
      return H2_PAL_ERR_FORMAT;
    peer->media_pending_opus_len = opus_len;
  }
  h2_pal_result_t result = h2_peer_network_enqueue_opus(
      peer, peer->media_pending_opus, peer->media_pending_opus_len);
  if (result == H2_PAL_OK)
    peer->media_pending_opus_len = 0u;
  return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

static h2_pal_result_t
h2_peer_network_channel_send(h2_pal_webrtc_channel_t *channel,
                             const uint8_t *data, size_t len, int is_text) {
  h2_pal_webrtc_peer_t *peer = channel->owner;
  const h2_pal_result_t failure = (h2_pal_result_t)atomic_load_explicit(
      &peer->network_transport_result, memory_order_acquire);
  if (failure != H2_PAL_OK)
    return failure;
  if (peer->closed) {
    return H2_PAL_ERR_CLOSED;
  }
  if (!channel->open || channel->terminal ||
      peer->state != H2_PAL_WEBRTC_PEER_CONNECTED) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_peer_channel_tx_push(channel, data, len, is_text);
}

static void h2_peer_network_channel_close(h2_pal_webrtc_channel_t *channel) {
  h2_pal_webrtc_peer_t *peer = channel->owner;
  const h2_peer_network_command_t command = {
      .type = H2_PEER_NETWORK_CHANNEL_CLOSE,
      .value.channel = channel,
  };
  if (h2_peer_network_call(peer, &command, NULL) != H2_PAL_OK) {
    return;
  }
}

static h2_pal_result_t
h2_peer_network_close_and_join(h2_pal_webrtc_peer_t *peer) {
  h2_peer_t *owner = peer->owner;
  if (!atomic_load_explicit(&peer->network_stop, memory_order_acquire)) {
    const h2_peer_network_command_t command = {
        .type = H2_PEER_NETWORK_PEER_CLOSE,
    };
    h2_pal_result_t rc = h2_peer_network_call(peer, &command, NULL);
    if (rc != H2_PAL_OK)
      return rc;
  }
  h2_pal_result_t rc = h2_pal_task_join(owner->config.task, peer->network_task);
  if (rc != H2_PAL_OK)
    return rc;
  peer->network_task = NULL;
  /* network_stop is published before the joined worker exits, so a waiting
   * poll leaves within one slice. Wait for it before destroying its queue. */
  while (atomic_load_explicit(&peer->network_poll_active, memory_order_acquire))
    (void)h2_pal_time_sleep_ms(owner->config.time, H2_PEER_POLL_WAIT_SLICE_MS);
  // Only the lifecycle caller edits the owner list, after the worker exits.
  h2_peer_network_unlink_created_peer(peer);
  h2_peer_network_discard_available(peer);
  h2_peer_network_destroy_resources(peer);
  h2_peer_connection_release(peer);
  return H2_PAL_OK;
}

static void h2_peer_network_peer_close(h2_pal_webrtc_peer_t *peer) {
  // A join failure retains the peer on its owner so destroy can retry safely.
  (void)h2_peer_network_close_and_join(peer);
}

static const h2_pal_webrtc_vtable_t h2_peer_webrtc_vtable = {
    .peer_create = h2_peer_network_peer_create,
    .peer_add_ice_server = h2_peer_network_add_ice_server,
    .peer_start_offer = h2_peer_network_start_offer,
    .peer_set_remote_sdp = h2_peer_network_set_remote_sdp,
    .peer_create_data_channel = h2_peer_network_create_data_channel,
    .peer_set_track = h2_peer_network_set_track,
    .peer_unset_track = h2_peer_network_unset_track,
    .peer_poll = h2_peer_network_poll,
    .peer_send_opus = h2_peer_network_send_opus,
    .channel_send = h2_peer_network_channel_send,
    .channel_close = h2_peer_network_channel_close,
    .peer_close = h2_peer_network_peer_close,
};

h2_pal_result_t h2_peer_create(const h2_peer_config_t *config,
                               h2_peer_t **out_peer) {
  if (out_peer != NULL) {
    *out_peer = NULL;
  }
  if (out_peer == NULL || !h2_peer_config_valid(config)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!h2_peer_production_config_supported(config)) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_peer_t bootstrap = {.config = *config};
  h2_peer_t *peer =
      (h2_peer_t *)h2_peer_control_alloc(&bootstrap, sizeof(*peer));
  if (peer == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  atomic_init(&peer->refs, 1u);
  peer->config = *config;
  peer->webrtc_api.user = peer;
  peer->webrtc_api.vtable = &h2_peer_webrtc_vtable;
  *out_peer = peer;
  return H2_PAL_OK;
}

const h2_pal_webrtc_api_t *h2_peer_webrtc_api(h2_peer_t *peer) {
  return peer == NULL || peer->destroying ? NULL : &peer->webrtc_api;
}

void h2_peer_destroy(h2_peer_t **peer) {
  if (peer == NULL || *peer == NULL) {
    return;
  }
  h2_peer_t *owner = *peer;
  owner->destroying = 1;
  while (owner->peers != NULL) {
    if (h2_peer_network_close_and_join(owner->peers) != H2_PAL_OK)
      return;
  }
  *peer = NULL;
  h2_peer_owner_release(owner);
}
