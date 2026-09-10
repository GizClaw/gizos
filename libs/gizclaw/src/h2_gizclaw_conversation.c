#include "h2_gizclaw_conversation.h"

#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_pcm_ring.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_session_internal.h"
#include "h2_gizclaw_task_names.h"
#include "h2_gizclaw_workspace.h"

#include "events/peer_event.pb.h"
#include "gzc_client.h"
#include "gzc_common.h"
#include "gzc_event.h"
#include "opus.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Label on our own input boundaries; the server echoes it on their end. */
#define H2_GIZCLAW_CONVERSATION_INPUT_LABEL "demo-home"

#define H2_GIZCLAW_CONVERSATION_OPUS_UPLINK_RING_ITEMS 8u
#define H2_GIZCLAW_CONVERSATION_OPUS_DOWNLINK_RING_ITEMS 32u
/* Decoded chunks one downlink wake may push into the Track. */
#define H2_GIZCLAW_CONVERSATION_DECODE_BURST_CHUNKS 8u
#define H2_GIZCLAW_CONVERSATION_OPUS_FRAME_SAMPLES 320u
#define H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES                               \
  (H2_GIZCLAW_CONVERSATION_OPUS_FRAME_SAMPLES * sizeof(int16_t))
#define H2_GIZCLAW_CONVERSATION_DECODE_MAX_SAMPLES 1920u

typedef enum h2_gizclaw_audio_message_kind {
  H2_GIZCLAW_AUDIO_MESSAGE_PCM = 0,
  H2_GIZCLAW_AUDIO_MESSAGE_OPUS,
  H2_GIZCLAW_AUDIO_MESSAGE_EOS,
} h2_gizclaw_audio_message_kind_t;

typedef struct h2_gizclaw_conversation_request_message {
  h2_gizclaw_audio_message_kind_t kind;
  size_t len;
  uint8_t data[H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES];
} h2_gizclaw_conversation_request_message_t;

typedef struct h2_gizclaw_audio_ring {
  h2_gizclaw_service_t *service;
  /* Serialises producers only (the downlink ring has two: the network task
   * and the poll that stages the terminal marker). Consumers never lock, and
   * the service mutex is never taken on either side. */
  h2_pal_mutex_t *lock;
  uint8_t *items;
  size_t item_size;
  size_t capacity;
  atomic_size_t write_index;
  atomic_size_t read_index;
  atomic_bool closed;
} h2_gizclaw_audio_ring_t;

typedef h2_pal_result_t (*conversation_generation_event_fn)(
    void *user, h2_gizclaw_conversation_request_t *request,
    const h2_gizclaw_conversation_event_t *event);
typedef void (*conversation_generation_completion_fn)(
    void *user, h2_gizclaw_conversation_request_t *request);

struct h2_gizclaw_conversation_request {
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_conversation_t *conversation;
  h2_gizclaw_audio_ring_t opus_uplink;
  h2_gizclaw_audio_ring_t opus_downlink;
  h2_pal_mutex_t *input_mutex;
  OpusEncoder *encoder;
  OpusDecoder *decoder;
  uint8_t capture[H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES];
  size_t capture_len;
  h2_gizclaw_pcm_input_t input; /* Protected by input_mutex. */
  h2_gizclaw_conversation_request_message_t encoded;
  bool encoded_pending;
  bool encoder_eos;
  int16_t decoded[H2_GIZCLAW_CONVERSATION_DECODE_MAX_SAMPLES];
  size_t decoded_len, decoded_offset;
  /* Samples the last real packet decoded to; a loss marker is concealed for
   * exactly that duration instead of the decoder's maximum frame. */
  int plc_frame_samples;
  /* Set by the decoder after the first Track write of a reply; the poll
   * stages one REPLY_AUDIO_STARTED for it and clears both at REPLY_DONE. */
  atomic_bool reply_audio_started;
  atomic_bool reply_media_open;
  atomic_bool playback_canceled;
  atomic_bool input_empty;
  bool reply_audio_notified;
  atomic_bool wire_ready;
  atomic_bool control_ready;
  conversation_generation_event_fn on_event;
  conversation_generation_completion_fn completion;
  void *user;
  char workspace_name[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  size_t workspace_name_len;
  uint64_t generation;
  int timeout_ms;
  uint64_t bos_started_at_ms;
  uint64_t audio_bos_started_at_ms;
  h2_gizclaw_conversation_request_message_t pending_downlink_message;
  h2_gizclaw_conversation_event_t pending_terminal_event;
  h2_gizclaw_conversation_event_t dispatch_event;
  char dispatch_text[H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES + 1u];
  char dispatch_error[65];
  bool notification_pending, notification_queued, notification_terminal;
  atomic_bool notification_done, notification_suppressed;
  h2_pal_result_t notification_result, notification_terminal_result;
  uint64_t identity;
  atomic_size_t queued_frames;
  atomic_size_t queued_bytes;
  atomic_size_t pcm_write_failures;
  atomic_size_t reply_frames;
  atomic_size_t reply_bytes;
  /* Frames queued before the downstream stream was replaced are stale: the
   * decoder drops them and clears the Track when it crosses this mark. */
  atomic_size_t discard_frames_before;
  atomic_bool discard_downlink_pending;
  size_t decoded_frames;
  h2_gizclaw_operation_result_t operation_result;
  char terminal_error_code[65];
  bool terminal_retryable;
  bool has_pending_downlink_message;
  atomic_bool committed;
  atomic_bool terminal;
  atomic_bool downlink_eos;
  atomic_bool media_uplink_eos;
  atomic_int audio_result;
  atomic_int cancel_source;
  bool media_attached;
  bool transport_committed;
  bool terminal_waiting_for_audio;
  bool reply_boundary_terminal;
  /* The staged REPLY_DONE ends a reply the server cut short (barge-in). */
  bool reply_interrupted;
};

/* One downstream stream as seen on the wire. The server owns its ID; it has
 * no relation to our input stream ID. */
typedef struct conversation_reply_route {
  // Server reply IDs are protocol-owned, not bounded by our input ID limit.
  char id[sizeof(((gzc_peer_event_t *)0)->payload.bos.stream_id)];
  char label[sizeof(((gzc_peer_event_t *)0)->payload.bos.label)];
  bool audio_open, audio_seen, audio_ended, ended;
} conversation_reply_route_t;

_Static_assert(
    sizeof(((conversation_reply_route_t *)0)->id) ==
            sizeof(((gzc_peer_event_t *)0)->payload.text_delta.stream_id) &&
        sizeof(((conversation_reply_route_t *)0)->id) ==
            sizeof(((gzc_peer_event_t *)0)->payload.text_done.stream_id) &&
        sizeof(((conversation_reply_route_t *)0)->id) ==
            sizeof(((gzc_peer_event_t *)0)->payload.eos.stream_id),
    "reply event stream ID capacities must match");

struct h2_gizclaw_conversation {
  h2_gizclaw_service_t *service;
  h2_gizclaw_conversation_request_t *service_request;
  h2_gizclaw_conversation_callback_fn callback;
  h2_gizclaw_conversation_completion_fn completion;
  void *callback_user;
  uint64_t next_generation;
  int service_mode;
  bool input_ended;
  h2_gizclaw_client_t *client;
  const h2_pal_mem_api_t *allocator;
  gzc_client_t *gzc;
  gzc_event_stream_t *events;
  uint64_t generation;
  char workspace_name[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  char stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  /* Upstream and downstream each have exactly one active stream. downstream
   * is whatever the server is currently sending us; a BOS with a new ID
   * replaces it and the old ID is ignored from then on. transcript is the
   * server's recognition of our own input and travels under our input ID. */
  conversation_reply_route_t downstream, transcript;
  uint64_t sequence;
  bool bos_sent;
  bool audio_bos_requested;
  bool audio_bos_sent;
  bool audio_eos_sent;
  bool input_ready;
  bool input_rejected;
  bool committed;
  bool canceled;
  bool terminal_pending;
  bool terminal_has_error;
  bool terminal_retryable;
  bool reply_interrupted;
  /* A downstream stream other than the active audio one has begun since
   * that audio stream started: the server is moving on to its next part. */
  bool newer_downstream;
  bool pending_peer_event;
  gzc_peer_event_t peer_event;
  char text[H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES + 1u];
  char error_code[65];
};

static int gzc_to_pal(int rc) {
  switch (rc) {
  case GZC_OK:
    return H2_PAL_OK;
  case GZC_ERR_INVALID_ARGUMENT:
    return H2_PAL_ERR_INVALID_ARG;
  case GZC_ERR_NO_MEMORY:
    return H2_PAL_ERR_NO_MEMORY;
  case GZC_ERR_TIMEOUT:
    return H2_PAL_ERR_TIMEOUT;
  case GZC_ERR_CLOSED:
    return H2_PAL_ERR_CLOSED;
  case GZC_ERR_UNSUPPORTED:
    return H2_PAL_ERR_UNSUPPORTED;
  case GZC_ERR_WOULD_BLOCK:
    return H2_PAL_ERR_WOULD_BLOCK;
  default:
    return H2_PAL_ERR_IO;
  }
}

static h2_pal_result_t audio_ring_init(h2_gizclaw_audio_ring_t *ring,
                                       h2_gizclaw_service_t *service,
                                       size_t item_size, size_t capacity) {
  if (ring == NULL || service == NULL || item_size == 0u || capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  memset(ring, 0, sizeof(*ring));
  ring->service = service;
  ring->item_size = item_size;
  ring->capacity = capacity;
  ring->items = h2_pal_mem_alloc(service->config.client_config->allocator,
                                 item_size * capacity);
  if (ring->items == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  const h2_pal_mutex_config_t lock_config = {
      .name = "$gizclaw/audio-ring",
      .allocator = service->config.client_config->allocator};
  const h2_pal_result_t lock_rc =
      h2_pal_mutex_create(service->config.sync, &lock_config, &ring->lock);
  if (lock_rc != H2_PAL_OK) {
    h2_pal_mem_free(service->config.client_config->allocator, ring->items);
    ring->items = NULL;
    return lock_rc;
  }
  atomic_init(&ring->write_index, 0u);
  atomic_init(&ring->read_index, 0u);
  atomic_init(&ring->closed, false);
  return H2_PAL_OK;
}

static h2_pal_result_t audio_ring_send(h2_gizclaw_audio_ring_t *ring,
                                       const void *item) {
  if (ring == NULL || ring->items == NULL || item == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_service_t *service = ring->service;
  h2_pal_result_t rc = ring->lock != NULL
                           ? h2_pal_mutex_lock(service->config.sync, ring->lock)
                           : H2_PAL_OK;
  if (rc != H2_PAL_OK)
    return rc;
  size_t write = atomic_load(&ring->write_index);
  size_t read = atomic_load(&ring->read_index);
  if (atomic_load(&ring->closed))
    rc = H2_PAL_ERR_CLOSED;
  else if (write - read >= ring->capacity)
    rc = H2_PAL_ERR_WOULD_BLOCK;
  else {
    memcpy(ring->items + (write % ring->capacity) * ring->item_size, item,
           ring->item_size);
    atomic_store(&ring->write_index, write + 1);
  }
  if (ring->lock != NULL)
    (void)h2_pal_mutex_unlock(service->config.sync, ring->lock);
  return rc;
}

static h2_pal_result_t audio_ring_recv_checked(h2_gizclaw_audio_ring_t *ring,
                                               void *out_item,
                                               size_t max_opus_bytes) {
  if (ring == NULL || ring->items == NULL || out_item == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  /* Single consumer: the indices are published with acquire/release, so the
   * realtime reader never waits on a lock. */
  h2_pal_result_t rc = H2_PAL_OK;
  size_t read = atomic_load(&ring->read_index);
  size_t write = atomic_load(&ring->write_index);
  if (read == write)
    rc =
        atomic_load(&ring->closed) ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_WOULD_BLOCK;
  else {
    memcpy(out_item, ring->items + (read % ring->capacity) * ring->item_size,
           ring->item_size);
    if (max_opus_bytes != SIZE_MAX) {
      const h2_gizclaw_conversation_request_message_t *message = out_item;
      if (message->kind == H2_GIZCLAW_AUDIO_MESSAGE_OPUS &&
          message->len > max_opus_bytes)
        rc = H2_PAL_ERR_INVALID_ARG;
    }
    if (rc == H2_PAL_OK)
      atomic_store(&ring->read_index, read + 1);
  }
  return rc;
}

static h2_pal_result_t audio_ring_recv(h2_gizclaw_audio_ring_t *ring,
                                       void *out_item) {
  return audio_ring_recv_checked(ring, out_item, SIZE_MAX);
}

static void audio_ring_close(h2_gizclaw_audio_ring_t *ring) {
  if (ring == NULL || ring->items == NULL ||
      atomic_exchange_explicit(&ring->closed, true, memory_order_acq_rel))
    return;
}

static void audio_ring_deinit(h2_gizclaw_audio_ring_t *ring) {
  if (ring == NULL || ring->items == NULL)
    return;
  if (ring->lock != NULL)
    (void)h2_pal_mutex_destroy(ring->service->config.sync, ring->lock);
  h2_pal_mem_free(ring->service->config.client_config->allocator, ring->items);
  memset(ring, 0, sizeof(*ring));
}

static h2_gizclaw_conversation_request_t *
media_request_acquire_tagged(h2_gizclaw_service_t *service, int tag) {
  if (service == NULL ||
      h2_pal_mutex_lock(service->config.sync, service->mutex) != H2_PAL_OK)
    return NULL;
  h2_gizclaw_conversation_request_t *request =
      atomic_load(&service->media_request);
  if (request != NULL) {
    atomic_fetch_add(&service->media_callback_refs, 1);
    atomic_store_explicit(&service->media_holder_tag, tag,
                          memory_order_relaxed);
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  return request;
}

#define media_request_acquire(service)                                         \
  media_request_acquire_tagged((service), __LINE__)

static void media_request_release(h2_gizclaw_service_t *service) {
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  atomic_fetch_sub(&service->media_callback_refs, 1);
  (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

h2_pal_result_t h2_gizclaw_conversation_media_attach(
    h2_gizclaw_service_t *service, h2_gizclaw_conversation_request_t *request) {
  if (service == NULL || request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_conversation_request_t *expected = NULL;
  if (service->pcm_track_unsetting ||
      atomic_load(&service->speech_request) != NULL ||
      service->audio_play != NULL ||
      !atomic_compare_exchange_strong_explicit(
          &service->media_request, &expected, request, memory_order_seq_cst,
          memory_order_seq_cst)) {
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    return H2_PAL_ERR_INVALID_STATE;
  }
  request->media_attached = true;
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  return H2_PAL_OK;
}

void h2_gizclaw_conversation_media_detach(
    h2_gizclaw_conversation_request_t *request) {
  if (request == NULL || !request->media_attached)
    return;
  h2_gizclaw_service_t *service = request->service;
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  h2_gizclaw_conversation_request_t *expected = request;
  (void)atomic_compare_exchange_strong(&service->media_request, &expected,
                                       NULL);
  request->media_attached = false;
  unsigned int waits = 0u;
  while (atomic_load(&service->media_callback_refs) != 0) {
    const h2_pal_result_t wait_rc = h2_pal_cond_wait(
        service->config.sync, service->progress_cond, service->mutex, 1000u);
    if (wait_rc == H2_PAL_ERR_TIMEOUT &&
        atomic_load(&service->media_callback_refs) != 0) {
      /* A media callback is holding the request for far longer than one
       * audio period. Name the last acquirer so the stall can be traced. */
      h2_gizclaw_service_log_request(
          service, H2_PAL_LOG_WARN, "conversation", "media_detach_wait",
          request->identity, H2_PAL_ERR_TIMEOUT,
          atomic_load_explicit(&service->media_holder_tag,
                               memory_order_relaxed),
          atomic_load(&service->media_callback_refs), ++waits);
    }
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

h2_pal_result_t
h2_gizclaw_service_media_read_opus(h2_gizclaw_service_t *service, uint8_t *opus,
                                   size_t capacity, size_t *out_len) {
  if (out_len != NULL)
    *out_len = 0u;
  if (service == NULL || opus == NULL || out_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_conversation_request_t *request = media_request_acquire(service);
  if (request == NULL)
    return H2_PAL_ERR_WOULD_BLOCK;
  /* Encoding may run after control BOS; RTP waits for the audio channel ACK.
   * An empty stream completes directly in the encoder without using RTP. */
  if (!atomic_load_explicit(&request->wire_ready, memory_order_acquire)) {
    media_request_release(service);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_gizclaw_conversation_request_message_t message = {0};
  h2_pal_result_t rc =
      audio_ring_recv_checked(&request->opus_uplink, &message, capacity);
  if (rc == H2_PAL_OK && message.kind == H2_GIZCLAW_AUDIO_MESSAGE_EOS) {
    atomic_store_explicit(&request->media_uplink_eos, true,
                          memory_order_release);
    rc = H2_PAL_ERR_WOULD_BLOCK;
  } else if (rc == H2_PAL_OK &&
             (message.kind != H2_GIZCLAW_AUDIO_MESSAGE_OPUS ||
              message.len == 0u || message.len > capacity)) {
    rc = H2_PAL_ERR_FORMAT;
  } else if (rc == H2_PAL_OK) {
    memcpy(opus, message.data, message.len);
    *out_len = message.len;
  }
  media_request_release(service);
  return rc == H2_PAL_ERR_CLOSED ? H2_PAL_ERR_WOULD_BLOCK : rc;
}

h2_pal_result_t
h2_gizclaw_service_media_write_opus(h2_gizclaw_service_t *service,
                                    const uint8_t *opus, size_t opus_len) {
  if (service == NULL || (opus == NULL && opus_len != 0u) ||
      opus_len > H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_conversation_request_t *request = media_request_acquire(service);
  if (request == NULL)
    return H2_PAL_OK;
  if (!atomic_load_explicit(&request->reply_media_open, memory_order_acquire) ||
      atomic_load_explicit(&request->playback_canceled, memory_order_acquire)) {
    media_request_release(service);
    return H2_PAL_OK;
  }
  h2_gizclaw_conversation_request_message_t message = {
      .kind = H2_GIZCLAW_AUDIO_MESSAGE_OPUS, .len = opus_len};
  if (opus_len != 0u)
    memcpy(message.data, opus, opus_len);
  h2_pal_result_t rc = audio_ring_send(&request->opus_downlink, &message);
  if (rc == H2_PAL_OK) {
    atomic_fetch_add_explicit(&request->reply_frames, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&request->reply_bytes, opus_len,
                              memory_order_relaxed);
  } else if (rc == H2_PAL_ERR_CLOSED) {
    rc = H2_PAL_OK;
  }
  media_request_release(service);
  return rc;
}

#if defined(H2_GIZCLAW_TESTING)
bool h2_gizclaw_test_audio_rings(void) {
  uint8_t pcm_storage[8] = {0};
  h2_gizclaw_pcm_ring_t pcm = {
      .bytes = pcm_storage,
      .capacity = sizeof(pcm_storage),
  };
  atomic_init(&pcm.write_index, 0u);
  atomic_init(&pcm.read_index, 0u);
  atomic_init(&pcm.closed, false);
  const uint8_t first[] = {1u, 2u, 3u, 4u, 5u, 6u};
  const uint8_t second[] = {7u, 8u, 9u, 10u, 11u, 12u};
  uint8_t actual[8] = {0};
  if (h2_gizclaw_pcm_ring_write(&pcm, first, sizeof(first)) != H2_PAL_OK ||
      h2_gizclaw_pcm_ring_read(&pcm, actual, 4u) != H2_PAL_OK ||
      memcmp(actual, first, 4u) != 0 ||
      h2_gizclaw_pcm_ring_write(&pcm, second, sizeof(second)) != H2_PAL_OK ||
      h2_gizclaw_pcm_ring_write(&pcm, first, 1u) != H2_PAL_ERR_WOULD_BLOCK ||
      h2_gizclaw_pcm_ring_read(&pcm, actual, sizeof(actual)) != H2_PAL_OK)
    return false;
  const uint8_t expected[] = {5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u};
  if (memcmp(actual, expected, sizeof(expected)) != 0)
    return false;

  uint32_t item_storage[2] = {0u};
  h2_gizclaw_audio_ring_t items = {
      .items = (uint8_t *)item_storage,
      .item_size = sizeof(item_storage[0]),
      .capacity = 2u,
  };
  atomic_init(&items.write_index, 0u);
  atomic_init(&items.read_index, 0u);
  atomic_init(&items.closed, false);
  const uint32_t one = 17u;
  const uint32_t two = 29u;
  uint32_t output = 0u;
  return audio_ring_send(&items, &one) == H2_PAL_OK &&
         audio_ring_send(&items, &two) == H2_PAL_OK &&
         audio_ring_send(&items, &one) == H2_PAL_ERR_WOULD_BLOCK &&
         audio_ring_recv(&items, &output) == H2_PAL_OK && output == one &&
         audio_ring_recv(&items, &output) == H2_PAL_OK && output == two &&
         audio_ring_recv(&items, &output) == H2_PAL_ERR_WOULD_BLOCK;
}
#endif

static h2_pal_result_t
audio_encode_opus(OpusEncoder *encoder, const int16_t *samples,
                  h2_gizclaw_conversation_request_message_t *message) {
  *message = (h2_gizclaw_conversation_request_message_t){
      .kind = H2_GIZCLAW_AUDIO_MESSAGE_OPUS};
  const int encoded =
      opus_encode(encoder, samples, H2_GIZCLAW_CONVERSATION_OPUS_FRAME_SAMPLES,
                  message->data, (opus_int32)sizeof(message->data));
  if (encoded <= 0)
    return H2_PAL_ERR_FORMAT;
  message->len = (size_t)encoded;
  return H2_PAL_OK;
}

static h2_pal_result_t
conversation_encode_step(h2_gizclaw_conversation_request_t *request,
                         const char **stage) {
  *stage = "pcm_prepare";
  h2_pal_result_t prepare_rc = h2_gizclaw_service_pcm_input_internal(
      request->service, &request->input, H2_GIZCLAW_PCM_INPUT_PREPARE, NULL, 0u,
      NULL);
  if (prepare_rc != H2_PAL_OK)
    return prepare_rc;
  if (!atomic_load_explicit(&request->control_ready, memory_order_acquire) ||
      request->encoder_eos)
    return H2_PAL_OK;
  if (request->encoder == NULL) {
    *stage = "encoder_create";
    int size = opus_encoder_get_size(1);
    request->encoder =
        size > 0 ? h2_pal_mem_alloc(request->service->client_config.allocator,
                                    (size_t)size)
                 : NULL;
    if (request->encoder == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    if (opus_encoder_init(request->encoder, 16000, 1, OPUS_APPLICATION_VOIP) !=
            OPUS_OK ||
        opus_encoder_ctl(request->encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK)
      return H2_PAL_ERR_FORMAT;
  }
  if (request->encoded_pending) {
    *stage = "opus_enqueue";
    h2_pal_result_t rc =
        audio_ring_send(&request->opus_uplink, &request->encoded);
    if (rc != H2_PAL_OK) {
      return rc;
    }

    request->encoded_pending = false;
    if (request->encoded.kind == H2_GIZCLAW_AUDIO_MESSAGE_EOS) {
      request->encoder_eos = true;
      return H2_PAL_OK;
    }
  }
  const bool ended =
      atomic_load_explicit(&request->committed, memory_order_acquire);
  if (request->capture_len < sizeof(request->capture)) {
    *stage = "pcm_read";
    size_t len = 0;
    h2_pal_result_t rc = h2_gizclaw_service_pcm_input_internal(
        request->service, &request->input, H2_GIZCLAW_PCM_INPUT_READ,
        request->capture + request->capture_len,
        sizeof(request->capture) - request->capture_len, &len);
    if (rc != H2_PAL_OK) {
      return rc;
    }
    request->capture_len += len;
    atomic_fetch_add(&request->queued_bytes, len);
  }
  if (request->capture_len == 0 && ended) {
    if (atomic_load_explicit(&request->queued_bytes, memory_order_acquire) == 0u) {
      request->encoder_eos = true;
      atomic_store_explicit(&request->media_uplink_eos, true,
                            memory_order_release);
      return H2_PAL_OK;
    }
    request->encoded = (h2_gizclaw_conversation_request_message_t){
        .kind = H2_GIZCLAW_AUDIO_MESSAGE_EOS};
    request->encoded_pending = true;
    return H2_PAL_OK;
  }
  if (request->capture_len < sizeof(request->capture) && !ended)
    return H2_PAL_OK;
  memset(request->capture + request->capture_len, 0,
         sizeof(request->capture) - request->capture_len);
  int16_t samples[H2_GIZCLAW_CONVERSATION_OPUS_FRAME_SAMPLES];
  for (size_t i = 0; i < H2_GIZCLAW_CONVERSATION_OPUS_FRAME_SAMPLES; ++i) {
    uint16_t value = (uint16_t)request->capture[i * 2] |
                     ((uint16_t)request->capture[i * 2 + 1] << 8);
    samples[i] = (int16_t)value;
  }
  *stage = "opus_encode";
  h2_pal_result_t rc =
      audio_encode_opus(request->encoder, samples, &request->encoded);
  if (rc == H2_PAL_OK) {
    request->capture_len = 0;
    request->encoded_pending = true;
    atomic_fetch_add(&request->queued_frames, 1);
  }
  return rc;
}

static h2_pal_result_t
conversation_decode_step(h2_gizclaw_conversation_request_t *request,
                         const char **stage) {
  *stage = "decoder_create";
  if (atomic_load_explicit(&request->playback_canceled, memory_order_acquire))
    return H2_PAL_ERR_WOULD_BLOCK;
  /* Downlink and server errors must progress while input awaits READY. */
  if (atomic_load_explicit(&request->downlink_eos, memory_order_acquire))
    return H2_PAL_OK;
  if (request->decoder == NULL) {
    int size = opus_decoder_get_size(1);
    request->decoder =
        size > 0 ? h2_pal_mem_alloc(request->service->client_config.allocator,
                                    (size_t)size)
                 : NULL;
    if (request->decoder == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    if (opus_decoder_init(request->decoder, 16000, 1) != OPUS_OK)
      return H2_PAL_ERR_FORMAT;
  }
  /* The downstream stream was replaced: everything queued or decoded up to
   * the mark belongs to the old stream. Drop it, and drop whatever the old
   * stream already had in the Track, exactly once. */
  if (atomic_load_explicit(&request->discard_downlink_pending,
                           memory_order_acquire) &&
      request->decoded_frames >= atomic_load_explicit(
                                     &request->discard_frames_before,
                                     memory_order_acquire)) {
    request->decoded_offset = request->decoded_len;
    h2_gizclaw_service_pcm_discard_downlink_internal(request->service);
    atomic_store_explicit(&request->discard_downlink_pending, false,
                          memory_order_release);
  }
  if (request->decoded_offset == request->decoded_len) {
    *stage = "opus_dequeue";
    h2_gizclaw_conversation_request_message_t input;
    h2_pal_result_t rc = audio_ring_recv(&request->opus_downlink, &input);
    if (rc != H2_PAL_OK)
      return rc;
    if (input.kind == H2_GIZCLAW_AUDIO_MESSAGE_EOS) {
      atomic_store_explicit(&request->downlink_eos, true, memory_order_release);
      return H2_PAL_OK;
    }
    if (input.kind != H2_GIZCLAW_AUDIO_MESSAGE_OPUS)
      return H2_PAL_ERR_FORMAT;
    ++request->decoded_frames;
    if (atomic_load_explicit(&request->discard_downlink_pending,
                             memory_order_acquire) &&
        request->decoded_frames <= atomic_load_explicit(
                                       &request->discard_frames_before,
                                       memory_order_acquire))
      return H2_PAL_OK; /* Stale frame of the replaced stream. */

    /* A zero-length item is the transport's RTP loss marker: ask Opus for
     * packet loss concealment over one packet's worth of samples. */
    *stage = "opus_decode";
    int samples = opus_decode(request->decoder, input.len ? input.data : NULL,
                              (opus_int32)input.len, request->decoded,
                              input.len != 0u
                                  ? (int)H2_GIZCLAW_CONVERSATION_DECODE_MAX_SAMPLES
                                  : request->plc_frame_samples,
                              0);
    if (samples <= 0)
      return H2_PAL_ERR_FORMAT;
    if (input.len != 0u)
      request->plc_frame_samples = samples;
    request->decoded_len = (size_t)samples * 2;
    request->decoded_offset = 0;
  }
  size_t len = request->decoded_len - request->decoded_offset;
  if (len > H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES)
    len = H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES;
  uint8_t pcm[H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES];
  for (size_t i = 0; i < len / 2; ++i) {
    uint16_t sample =
        (uint16_t)request->decoded[request->decoded_offset / 2 + i];
    pcm[i * 2] = (uint8_t)sample;
    pcm[i * 2 + 1] = (uint8_t)(sample >> 8);
  }
  {
    *stage = "pcm_write";
    h2_pal_result_t rc = h2_pal_mutex_lock(
        request->service->config.sync, request->input_mutex);
    if (rc != H2_PAL_OK)
      return rc;
    const bool canceled = atomic_load_explicit(&request->playback_canceled,
                                               memory_order_acquire);
    if (!canceled)
      rc = h2_gizclaw_service_pcm_write_internal(request->service, pcm, len);
    (void)h2_pal_mutex_unlock(request->service->config.sync, request->input_mutex);
    if (rc != H2_PAL_OK)
      return rc;
    if (canceled)
      return H2_PAL_ERR_WOULD_BLOCK;
    /* The Track owns playback from here. The app learns that this reply is
     * audible through one REPLY_AUDIO_STARTED, not through a copy per chunk. */
    atomic_store_explicit(&request->reply_audio_started, true,
                          memory_order_release);
  }
  request->decoded_offset += len;
  return H2_PAL_OK;
}

static void record_audio_result(h2_gizclaw_conversation_request_t *request,
                                h2_pal_result_t result, const char *stage) {
  if (result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK ||
      result == H2_PAL_ERR_TIMEOUT)
    return;
  int expected = H2_PAL_OK;
  if (!atomic_compare_exchange_strong(&request->audio_result, &expected, result))
    return;
  /* Exactly the first fatal audio result. The caller has released input_mutex;
   * read only immutable identifiers and atomic counters shared with control. */
  char message[384];
  (void)snprintf(message, sizeof(message),
      "request=conversation stage=audio_worker_failed phase=%s rc=%d "
      "identity=%llu generation=%llu wire_ready=%d committed=%d "
      "media_eos=%d frames=%zu bytes=%zu",
      stage, (int)result, (unsigned long long)request->identity,
      (unsigned long long)request->generation, atomic_load(&request->wire_ready),
      atomic_load(&request->committed), atomic_load(&request->media_uplink_eos),
      atomic_load(&request->queued_frames), atomic_load(&request->queued_bytes));
  (void)h2_pal_log_write(request->service->client_config.log, H2_PAL_LOG_ERROR,
                         "gizclaw", message);
}

void h2_gizclaw_conversation_uplink_step_internal(
    h2_gizclaw_service_t *service) {
  h2_gizclaw_conversation_request_t *request = media_request_acquire(service);
  if (request == NULL)
    return;
  const char *stage = "input_lock";
  h2_pal_result_t rc =
      h2_pal_mutex_lock(service->config.sync, request->input_mutex);
  if (rc == H2_PAL_OK) {
    if (atomic_load(&request->audio_result) == H2_PAL_OK)
      rc = conversation_encode_step(request, &stage);
    (void)h2_pal_mutex_unlock(service->config.sync, request->input_mutex);
  }
  record_audio_result(request, rc, stage);
  media_request_release(service);
}

void h2_gizclaw_conversation_downlink_step_internal(
    h2_gizclaw_service_t *service) {
  h2_gizclaw_conversation_request_t *request = media_request_acquire(service);
  if (request == NULL)
    return;
  /* One decoded chunk per wake caps this stage at roughly real time, so any
   * wake lost to scheduling leaves the Opus ring fuller for good and the PCM
   * Track intermittently dry. While the ring has a backlog and the Track has
   * room, keep decoding; the Track's own depth bounds the burst and its
   * WOULD_BLOCK ends it, so the speaker still paces playback. */
  for (unsigned int chunk = 0u;
       chunk < H2_GIZCLAW_CONVERSATION_DECODE_BURST_CHUNKS &&
       atomic_load(&request->audio_result) == H2_PAL_OK;
       ++chunk) {
    const char *stage = "decode";
    const h2_pal_result_t rc = conversation_decode_step(request, &stage);
    record_audio_result(request, rc, stage);
    if (rc != H2_PAL_OK)
      break;
  }
  media_request_release(service);
}

static bool event_transport_failed(int rc) {
  return rc != GZC_OK && rc != GZC_ERR_TIMEOUT && rc != GZC_ERR_WOULD_BLOCK;
}

static bool valid_workspace(h2_gizclaw_str_t workspace_name) {
  return workspace_name.data != NULL && workspace_name.len > 0u &&
         workspace_name.len <= H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES &&
         memchr(workspace_name.data, '\0', workspace_name.len) == NULL;
}

static const char *peer_event_stream_id(const gzc_peer_event_t *event) {
  if (event == NULL)
    return NULL;
  switch (event->type) {
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_AUDIO_INPUT_READY:
    return event->payload.audio_input_ready.stream_id;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS:
    return event->payload.bos.stream_id;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA:
    return event->payload.text_delta.stream_id;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE:
    return event->payload.text_done.stream_id;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS:
    return event->payload.eos.stream_id;
  default:
    return NULL;
  }
}

static const char *peer_event_label(const gzc_peer_event_t *event) {
  if (event == NULL)
    return NULL;
  switch (event->type) {
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS:
    return event->payload.bos.label;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA:
    return event->payload.text_delta.label;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE:
    return event->payload.text_done.label;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS:
    return event->payload.eos.label;
  default:
    return NULL;
  }
}

static bool stream_id_matches(const char *actual, const char *expected) {
  if (actual == NULL || expected == NULL)
    return false;
  const size_t expected_len = strlen(expected);
  return strcmp(actual, expected) == 0 ||
         (expected_len > 0u && strncmp(actual, expected, expected_len) == 0 &&
          actual[expected_len] == ':');
}

static bool event_is_audio_boundary(const gzc_peer_event_t *event) {
  return (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS &&
          event->payload.bos.kind ==
              gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO) ||
         (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS &&
          event->payload.eos.kind ==
              gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO);
}

/* Events that describe the server's view of our upstream: the recognised
 * transcript, and READY, the echoed end or the rejection of our own input
 * (our input stream ID with the label we sent, or without audio kind).
 * Everything else is downstream, even under our input's ID: a server is
 * free to reuse that ID for its own audio stream. Exact match only: a
 * server stream is also free to look like "<input>:something". */
static bool event_is_upstream_side(const h2_gizclaw_conversation_t *conversation,
                                   const gzc_peer_event_t *event) {
  const char *label = peer_event_label(event);
  const char *id = peer_event_stream_id(event);
  if (label != NULL && strcmp(label, "transcript") == 0)
    return true;
  if (id == NULL || conversation->stream_id[0] == '\0' ||
      strcmp(id, conversation->stream_id) != 0)
    return false;
  if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS)
    return false;
  if (!event_is_audio_boundary(event))
    return true;
  return label == NULL || label[0] == '\0' ||
         strcmp(label, H2_GIZCLAW_CONVERSATION_INPUT_LABEL) == 0;
}

static conversation_reply_route_t *
conversation_reply_route(h2_gizclaw_conversation_t *conversation,
                         const gzc_peer_event_t *event) {
  return event_is_upstream_side(conversation, event) ? &conversation->transcript
                                                     : &conversation->downstream;
}

/* The server cut the active downstream audio stream short
 * (STREAM_INTERRUPTED): the user spoke over it, or the server moved on to
 * its next stream. Either way it is only the end of that stream, never an
 * error for the turn. */
static bool reply_interruptible(const h2_gizclaw_conversation_t *conversation,
                                const conversation_reply_route_t *route) {
  (void)conversation;
  return route == &conversation->downstream;
}

/* Stale playback of the active downstream stream: everything it queued so
 * far, and whatever it already wrote to the Track, is dropped by the
 * decoder once it reaches this mark. */
static void discard_downstream_playback(
    h2_gizclaw_conversation_t *conversation) {
  h2_gizclaw_conversation_request_t *request = conversation->service_request;
  if (request == NULL)
    return;
  atomic_store_explicit(&request->reply_media_open, false,
                        memory_order_release);
  atomic_store_explicit(
      &request->discard_frames_before,
      atomic_load_explicit(&request->reply_frames, memory_order_acquire),
      memory_order_release);
  atomic_store_explicit(&request->discard_downlink_pending, true,
                        memory_order_release);
}

static void finish_interrupted_reply(h2_gizclaw_conversation_t *conversation,
                                     conversation_reply_route_t *route) {
  route->audio_open = false;
  route->audio_ended = true;
  route->ended = true;
  conversation->terminal_pending = true;
  conversation->terminal_has_error = false;
  conversation->reply_interrupted = true;
}

/* Upstream and downstream each have one active stream, and only audio
 * carries state. Text events are forwarded for display but never gate
 * anything: there are no subtitles to wait for. Streaming is lossy and
 * unordered at the edges, so be lenient: a new downstream audio ID replaces
 * the active one (its stragglers are ignored), a duplicate EOS is harmless,
 * and an EOS for an audio stream we never saw is nothing to act on. */
static bool accepts_peer_event(h2_gizclaw_conversation_t *conversation,
                               const gzc_peer_event_t *event) {
  if (conversation == NULL || event == NULL)
    return false;
  if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_AUDIO_INPUT_READY) {
    const char *id = event->payload.audio_input_ready.stream_id;
    return conversation->bos_sent && !conversation->canceled &&
           !conversation->committed && !conversation->input_rejected &&
           memchr(id, '\0', sizeof(event->payload.audio_input_ready.stream_id)) != NULL &&
           strcmp(id, conversation->stream_id) == 0;
  }
  const char *id = peer_event_stream_id(event);
  if (id == NULL)
    return false;
  const char *id_end = memchr(id, '\0', sizeof(conversation->downstream.id));
  if (id_end == NULL || id_end == id)
    return false;
  const size_t id_len = (size_t)(id_end - id);
  const char *label = peer_event_label(event);

  if (event_is_upstream_side(conversation, event)) {
    /* Our input's own boundaries and transcript: always consumed. */
    conversation_reply_route_t *route = &conversation->transcript;
    if (route->id[0] == '\0' || !stream_id_matches(id, route->id)) {
      memset(route, 0, sizeof(*route));
      memcpy(route->id, id, id_len + 1u);
    }
    if (label != NULL)
      (void)snprintf(route->label, sizeof(route->label), "%s", label);
    if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS)
      route->ended = true;
    return true;
  }
  if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS &&
      conversation->downstream.id[0] != '\0' &&
      !conversation->downstream.ended &&
      !stream_id_matches(id, conversation->downstream.id))
    conversation->newer_downstream = true;
  if (!event_is_audio_boundary(event)) {
    /* Text and other non-audio downstream events: forward, no state. */
    return true;
  }

  conversation_reply_route_t *route = &conversation->downstream;
  const bool same = route->id[0] != '\0' && stream_id_matches(id, route->id);
  if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS) {
    if (!same) {
      /* A new downstream audio stream replaces the active one; whatever the
       * old one still has queued or playing is stale. Close the media gate
       * first so the mark covers everything the old stream managed to queue;
       * the new BOS reopens it. */
      if (route->id[0] != '\0' && route->audio_open)
        discard_downstream_playback(conversation);
      memset(route, 0, sizeof(*route));
      memcpy(route->id, id, id_len + 1u);
      conversation->newer_downstream = false;
    }
    if (label != NULL && label[0] != '\0')
      (void)snprintf(route->label, sizeof(route->label), "%s", label);
    route->audio_open = true;
    route->audio_seen = true;
    route->audio_ended = false;
    route->ended = false;
    return true;
  }
  /* EOS of the active audio stream ends the turn. A duplicate end, a
   * straggler from a replaced stream and the end of a stream we never
   * played carry nothing to act on, errors included. One exception: with
   * no downstream audio active, a server that answers under our input's
   * own ID closes an audio-less turn with that EOS; bind it so the
   * boundary is dispatched as the turn's end. */
  if (same) {
    if (route->ended)
      return false;
  } else if ((route->id[0] != '\0' && !route->ended) ||
             strcmp(id, conversation->stream_id) != 0) {
    return false;
  } else {
    memset(route, 0, sizeof(*route));
    memcpy(route->id, id, id_len + 1u);
    if (label != NULL && label[0] != '\0')
      (void)snprintf(route->label, sizeof(route->label), "%s", label);
  }
  route->audio_open = false;
  route->audio_ended = true;
  route->ended = true;
  return true;
}

bool h2_gizclaw_conversation_accepts_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event) {
  return accepts_peer_event(conversation, event);
}

bool h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
    h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL)
    return false;
  const bool interrupted = conversation->reply_interrupted;
  conversation->reply_interrupted = false;
  return interrupted;
}

void h2_gizclaw_conversation_describe_peer_event_internal(
    const h2_gizclaw_conversation_t *conversation,
    const gzc_peer_event_t *event, char *out, size_t cap) {
  if (out == NULL || cap == 0u)
    return;
  if (conversation == NULL || event == NULL) {
    out[0] = '\0';
    return;
  }
  const conversation_reply_route_t *route =
      event_is_upstream_side(conversation, event) ? &conversation->transcript
                                                  : &conversation->downstream;
  const char *label = peer_event_label(event);
  const char *id = peer_event_stream_id(event);
  int kind = -1;
  const char *mime = "-";
  if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS) {
    kind = (int)event->payload.bos.kind;
    mime = event->payload.bos.mime_type;
  } else if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS) {
    kind = (int)event->payload.eos.kind;
    mime = event->payload.eos.mime_type;
  }
  (void)snprintf(
      out, cap,
      "generation=%llu input=%.24s ready=%d committed=%d canceled=%d "
      "label=%.12s id=%.40s kind=%d mime=%.16s error=%d route=%.16s ended=%d "
      "pending=%d",
      (unsigned long long)conversation->generation, conversation->stream_id,
      conversation->input_ready, conversation->committed, conversation->canceled,
      label != NULL ? label : "-", id != NULL ? id : "-", kind,
      mime[0] != '\0' ? mime : "-",
      event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS
          ? (int)event->payload.eos.has_error : 0,
      route->id, route->ended, conversation->terminal_pending);
}

bool h2_gizclaw_conversation_has_pending_peer_event_internal(
    const h2_gizclaw_conversation_t *conversation) {
  return conversation != NULL && conversation->pending_peer_event;
}

void h2_gizclaw_conversation_enqueue_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event) {
  if (conversation == NULL || event == NULL)
    return;
  if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_AUDIO_INPUT_READY) {
    if (conversation->audio_bos_sent && accepts_peer_event(conversation, event))
      conversation->input_ready = true;
    return;
  }
  if (conversation->pending_peer_event) {
    /* The poll has not consumed the previous event yet. Losing a boundary
     * here leaves the reply waiting forever, so make the loss visible. */
    if (conversation->service != NULL &&
        conversation->service->client_config.log != NULL) {
      char message[96];
      (void)snprintf(message, sizeof(message),
                     "event=peer_dropped type=%d pending_type=%d",
                     (int)event->type, (int)conversation->peer_event.type);
      (void)h2_pal_log_write(conversation->service->client_config.log,
                             H2_PAL_LOG_ERROR, "gizclaw/conversation", message);
    }
    return;
  }
  if (!conversation->input_ready &&
      event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS &&
      event->payload.eos.has_error)
    conversation->input_rejected = true;
  if (conversation->service_request != NULL) {
    const bool bos = event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS &&
        event->payload.bos.kind == gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO;
    const bool eos = event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS &&
        event->payload.eos.kind == gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO;
    if (bos || eos) {
      h2_gizclaw_conversation_request_t *request = conversation->service_request;
      atomic_store_explicit(&request->reply_media_open, bos, memory_order_release);
      h2_gizclaw_service_log_request(
          request->service, H2_PAL_LOG_DEBUG, "conversation",
          bos ? "reply_audio_bos" : "reply_audio_eos", request->identity,
          H2_PAL_OK, 0, atomic_load(&request->reply_frames),
          atomic_load(&request->reply_bytes));
    }
  }
  conversation->peer_event = *event;
  conversation->pending_peer_event = true;
}

static int send_boundary(h2_gizclaw_conversation_t *conversation, bool end,
                         bool audio,
                         uint64_t timestamp_ms, const char *error_code) {
  if (conversation == NULL || conversation->events == NULL ||
      !h2_gizclaw_client_conversation_active_internal(conversation->client,
                                                      conversation))
    return H2_PAL_ERR_INVALID_STATE;
  gzc_peer_event_t event = gizclaw_events_v1_PeerEvent_init_zero;
  event.version = GZC_PEER_EVENT_VERSION;
  if (!end) {
    event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
    event.which_payload = gizclaw_events_v1_PeerEvent_bos_tag;
    (void)snprintf(event.payload.bos.stream_id,
                   sizeof(event.payload.bos.stream_id), "%s",
                   conversation->stream_id);
    event.payload.bos.sequence = conversation->sequence;
    event.payload.bos.timestamp_unix_ms = (int64_t)timestamp_ms;
    event.payload.bos.kind = audio
        ? gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO
        : gizclaw_events_v1_StreamKind_STREAM_KIND_UNSPECIFIED;
    (void)snprintf(event.payload.bos.label, sizeof(event.payload.bos.label),
                   "%s", H2_GIZCLAW_CONVERSATION_INPUT_LABEL);
    (void)snprintf(event.payload.bos.mime_type,
                   sizeof(event.payload.bos.mime_type), "%s", audio ? "audio/opus" : "");
  } else {
    event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
    event.which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
    (void)snprintf(event.payload.eos.stream_id,
                   sizeof(event.payload.eos.stream_id), "%s",
                   conversation->stream_id);
    event.payload.eos.sequence = conversation->sequence;
    event.payload.eos.timestamp_unix_ms = (int64_t)timestamp_ms;
    event.payload.eos.kind = audio
        ? gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO
        : gizclaw_events_v1_StreamKind_STREAM_KIND_UNSPECIFIED;
    (void)snprintf(event.payload.eos.label, sizeof(event.payload.eos.label),
                   "%s", H2_GIZCLAW_CONVERSATION_INPUT_LABEL);
    (void)snprintf(event.payload.eos.mime_type,
                   sizeof(event.payload.eos.mime_type), "%s", audio ? "audio/opus" : "");
    if (error_code != NULL) {
      event.payload.eos.has_error = true;
      (void)snprintf(event.payload.eos.error.code,
                     sizeof(event.payload.eos.error.code), "%s", error_code);
      event.payload.eos.error.retryable = true;
    }
  }
  const int gzc_rc =
      h2_gizclaw_event_stream_send_internal(conversation->events, &event);
  if (gzc_rc == GZC_OK)
    ++conversation->sequence;
  if (event_transport_failed(gzc_rc))
    h2_gizclaw_client_event_failure_internal(conversation->client,
                                             conversation);
  return gzc_to_pal(gzc_rc);
}

int h2_gizclaw_conversation_wire_open_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t workspace_name,
    uint64_t generation, int timeout_ms,
    h2_gizclaw_conversation_t **out_conversation) {
  if (client == NULL || out_conversation == NULL ||
      !valid_workspace(workspace_name) || timeout_ms <= 0) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_conversation = NULL;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  gzc_client_t *gzc = h2_gizclaw_client_gzc_internal(client);
  if (allocator == NULL || gzc == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  h2_gizclaw_conversation_t *conversation =
      h2_pal_mem_alloc(allocator, sizeof(*conversation));
  if (conversation == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(conversation, 0, sizeof(*conversation));
  conversation->client = client;
  conversation->allocator = allocator;
  conversation->gzc = gzc;
  conversation->generation = generation;
  memcpy(conversation->workspace_name, workspace_name.data, workspace_name.len);
  conversation->workspace_name[workspace_name.len] = '\0';
  uint64_t stream_sequence = 0u;
  int rc = h2_gizclaw_client_conversation_acquire_internal(
      client, conversation, &conversation->events, &stream_sequence);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, conversation);
    return rc;
  }
  const int stream_len =
      snprintf(conversation->stream_id, sizeof(conversation->stream_id),
               "demo-%llu", (unsigned long long)stream_sequence);
  if (stream_len <= 0 ||
      (size_t)stream_len >= sizeof(conversation->stream_id)) {
    h2_gizclaw_client_conversation_release_internal(client, conversation);
    h2_pal_mem_free(allocator, conversation);
    return H2_PAL_ERR_INVALID_ARG;
  }
  (void)timeout_ms;
  rc = send_boundary(conversation, false, false, 0u, NULL);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK &&
      rc != H2_PAL_ERR_TIMEOUT) {
    h2_gizclaw_client_conversation_release_internal(client, conversation);
    h2_pal_mem_free(allocator, conversation);
    return rc;
  }
  conversation->bos_sent = rc == H2_PAL_OK;
  *out_conversation = conversation;
  return conversation->bos_sent ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
}

int h2_gizclaw_conversation_wire_begin_audio_internal(
    h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!conversation->bos_sent || conversation->committed || conversation->canceled)
    return H2_PAL_ERR_INVALID_STATE;
  conversation->audio_bos_requested = true;
  if (conversation->audio_bos_sent)
    return H2_PAL_OK;
  const int rc = send_boundary(conversation, false, true, 0u, NULL);
  if (rc == H2_PAL_OK)
    conversation->audio_bos_sent = true;
  return rc;
}

bool h2_gizclaw_conversation_wire_input_ready_internal(
    const h2_gizclaw_conversation_t *conversation) {
  return conversation != NULL && conversation->input_ready &&
         !conversation->committed && !conversation->canceled &&
         h2_gizclaw_client_conversation_active_internal(conversation->client,
                                                        conversation);
}

int h2_gizclaw_conversation_wire_finish_input_internal(
    h2_gizclaw_conversation_t *conversation, uint64_t timestamp_ms) {
  if (conversation == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (conversation->canceled)
    return H2_PAL_ERR_CLOSED;
  if (conversation->committed)
    return H2_PAL_OK;
  if (conversation->audio_bos_requested && !conversation->audio_bos_sent)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (!conversation->bos_sent ||
      (conversation->audio_bos_sent && !conversation->input_ready))
    return H2_PAL_ERR_INVALID_STATE;
  if (conversation->audio_bos_sent && !conversation->audio_eos_sent) {
    const int rc = send_boundary(conversation, true, true, timestamp_ms, NULL);
    if (rc != H2_PAL_OK)
      return rc;
    conversation->audio_eos_sent = true;
  }
  const int rc = send_boundary(conversation, true, false, timestamp_ms, NULL);
  if (rc == H2_PAL_OK) {
    conversation->committed = true;
    conversation->input_ready = false;
  }
  return rc;
}

static void copy_text(char *out, size_t capacity, const char *text,
                      size_t *out_len) {
  size_t len = 0u;
  while (len + 1u < capacity && text[len] != '\0')
    ++len;
  memcpy(out, text, len);
  out[len] = '\0';
  *out_len = len;
}

int h2_gizclaw_conversation_wire_poll_internal(
    h2_gizclaw_conversation_t *conversation, int timeout_ms,
    h2_gizclaw_conversation_event_t *out_event) {
  if (conversation == NULL || out_event == NULL || timeout_ms < 0)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_event, 0, sizeof(*out_event));
  if (conversation->canceled)
    return H2_PAL_ERR_CLOSED;
  if (!h2_gizclaw_client_conversation_active_internal(conversation->client,
                                                      conversation))
    return H2_PAL_ERR_CLOSED;
  if (conversation->terminal_pending) {
    conversation->terminal_pending = false;
    if (conversation->terminal_has_error) {
      out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_ERROR;
      out_event->generation = conversation->generation;
      out_event->error_code = conversation->error_code;
      out_event->retryable = conversation->terminal_retryable;
    } else {
      out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE;
      out_event->generation = conversation->generation;
    }
    return H2_PAL_OK;
  }

  if (!conversation->pending_peer_event) {
    const h2_pal_result_t dispatch_rc =
        (h2_pal_result_t)h2_gizclaw_client_dispatch_event(
            conversation->client, timeout_ms, NULL, NULL);
    if (dispatch_rc != H2_PAL_OK)
      return dispatch_rc;
  }
  if (!conversation->pending_peer_event)
    return H2_PAL_ERR_WOULD_BLOCK;
  const gzc_peer_event_t event = conversation->peer_event;
  conversation->pending_peer_event = false;
  size_t text_len = 0u;
  *out_event = (h2_gizclaw_conversation_event_t){
      .generation = conversation->generation,
  };
  switch (event.type) {
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA:
    copy_text(conversation->text, sizeof(conversation->text),
              event.payload.text_delta.text, &text_len);
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA;
    out_event->text = conversation->text;
    out_event->text_len = text_len;
    return H2_PAL_OK;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE:
    copy_text(conversation->text, sizeof(conversation->text),
              event.payload.text_done.text, &text_len);
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE;
    out_event->text = conversation->text;
    out_event->text_len = text_len;
    /* Text is display only; the turn ends with the downstream audio. */
    return H2_PAL_OK;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS:
    /* Only the active downstream audio stream ending finishes a turn. Our
     * own input's EOS, a transcript and text streams carry no state; an
     * error on any of them is still terminal. accepts_peer_event() already
     * bound the stream, so compare against the ID it recorded. */
    if (!event.payload.eos.has_error &&
        (!event_is_audio_boundary(&event) ||
         strcmp(event.payload.eos.label, "transcript") == 0 ||
         strcmp(event.payload.eos.stream_id, conversation->downstream.id) != 0))
      return H2_PAL_OK;
    if (event.payload.eos.has_error &&
        strcmp(event.payload.eos.error.code,
               H2_GIZCLAW_CONVERSATION_ERROR_STREAM_INTERRUPTED) == 0 &&
        reply_interruptible(conversation,
                            conversation_reply_route(conversation, &event))) {
      if (conversation->newer_downstream) {
        /* Handoff: the server already started its next stream. Drop what
         * is left of this one and keep the turn open for the next audio. */
        conversation->newer_downstream = false;
        discard_downstream_playback(conversation);
      } else {
        finish_interrupted_reply(conversation,
                                 conversation_reply_route(conversation, &event));
      }
      out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_NONE;
      return H2_PAL_OK;
    }
    conversation->terminal_pending = true;
    conversation->reply_interrupted = false;
    if (event.payload.eos.has_error) {
      (void)snprintf(conversation->error_code, sizeof(conversation->error_code),
                     "%s", event.payload.eos.error.code);
      conversation->terminal_has_error = true;
      conversation->terminal_retryable = event.payload.eos.error.retryable;
    }
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_NONE;
    return H2_PAL_OK;
  default:
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_NONE;
    return H2_PAL_OK;
  }
}

static void conversation_wire_cancel(h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL || conversation->canceled)
    return;
  if (conversation->bos_sent && !conversation->committed &&
      conversation->events != NULL)
    (void)send_boundary(conversation, true, false, 0u, "canceled");
  conversation->canceled = true;
  conversation->input_ready = false;
}

void h2_gizclaw_conversation_wire_destroy_internal(
    h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL)
    return;
  conversation_wire_cancel(conversation);
  h2_gizclaw_client_conversation_release_internal(conversation->client,
                                                  conversation);
  conversation->client = NULL;
  conversation->gzc = NULL;
  conversation->events = NULL;
  h2_pal_mem_free(conversation->allocator, conversation);
}

void h2_gizclaw_conversation_invalidate_internal(
    h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL)
    return;
  conversation->client = NULL;
  conversation->gzc = NULL;
  conversation->events = NULL;
  conversation->input_ready = false;
  conversation->canceled = true;
}

static void conversation_request_dispatch_event(void *user) {
  h2_gizclaw_conversation_request_t *request = user;
  h2_pal_result_t rc = H2_PAL_OK;
  const bool suppressed = atomic_load_explicit(
      &request->notification_suppressed, memory_order_acquire);
  if (!suppressed)
    rc = request->on_event(request->user, request, &request->dispatch_event);
  if (request->dispatch_event.kind != H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA)
    h2_gizclaw_service_log_request(
        request->service, rc == H2_PAL_OK ? H2_PAL_LOG_DEBUG : H2_PAL_LOG_ERROR,
        "conversation", "hook_dispatched",
        request->identity, rc, (int)request->dispatch_event.kind * 10 +
                                  (suppressed ? 1 : 0),
        request->notification_terminal, request->dispatch_event.generation);
  /* Hooks observe an already accepted event, not a retryable transport write.
   */
  request->notification_result =
      rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT || rc > 0
          ? H2_PAL_ERR_IO
          : rc;
  atomic_store_explicit(&request->notification_done, true,
                        memory_order_release);
}

static h2_pal_result_t
conversation_notification_step(h2_gizclaw_conversation_request_t *request) {
  if (!request->notification_queued) {
    h2_pal_result_t rc = h2_gizclaw_service_post_internal(
        request->service, conversation_request_dispatch_event, request);
    if (rc != H2_PAL_OK)
      return rc;
    request->notification_queued = true;
  }
  if (!atomic_load_explicit(&request->notification_done, memory_order_acquire))
    return H2_PAL_ERR_WOULD_BLOCK;
  request->notification_pending = false;
  return request->notification_result;
}

static h2_pal_result_t
conversation_queue_notification(h2_gizclaw_conversation_request_t *request,
                                bool terminal) {
  if (request->on_event == NULL)
    return H2_PAL_OK;
  /* Event projections borrow wire state. The network owner may cancel and
   * destroy that state while an application hook is running, so copy views. */
  if (request->dispatch_event.text != NULL) {
    size_t len = request->dispatch_event.text_len;
    if (len >= sizeof(request->dispatch_text))
      return H2_PAL_ERR_FORMAT;
    memcpy(request->dispatch_text, request->dispatch_event.text, len);
    request->dispatch_text[len] = '\0';
    request->dispatch_event.text = request->dispatch_text;
  }
  if (request->dispatch_event.error_code != NULL) {
    snprintf(request->dispatch_error, sizeof(request->dispatch_error), "%s",
             request->dispatch_event.error_code);
    request->dispatch_event.error_code = request->dispatch_error;
  }
  request->notification_pending = true;
  request->notification_queued = false;
  request->notification_terminal = terminal;
  atomic_store_explicit(&request->notification_done, false,
                        memory_order_release);
  /* Stage one immutable event. A full FIFO is retried on a later network tick.
   */
  return H2_PAL_ERR_WOULD_BLOCK;
}

static void
conversation_request_close_at(h2_gizclaw_conversation_request_t *request,
                              int line) {
  h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_DEBUG,
                                 "conversation", "close", request->identity,
                                 H2_PAL_OK, line, request->queued_frames,
                                 request->queued_bytes);
  atomic_store_explicit(&request->notification_suppressed, true,
                        memory_order_release);
  h2_gizclaw_conversation_media_detach(request);
  audio_ring_close(&request->opus_uplink);
  audio_ring_close(&request->opus_downlink);
  if (request->conversation == NULL)
    return;
  h2_gizclaw_conversation_wire_destroy_internal(request->conversation);
  request->conversation = NULL;
}

#define conversation_request_close(request)                                    \
  conversation_request_close_at((request), __LINE__)

/* A non-terminal REPLY_DONE reached the app: the next reply of this
 * generation decodes and announces its own audio start. */
static void conversation_reply_boundary_dispatched(
    h2_gizclaw_conversation_request_t *request) {
  request->reply_audio_notified = false;
  atomic_store_explicit(&request->reply_audio_started, false,
                        memory_order_release);
  atomic_store_explicit(&request->downlink_eos, false, memory_order_release);
}

static void log_conversation_state(h2_gizclaw_conversation_request_t *request,
                                   const char *stage, h2_pal_result_t rc,
                                   h2_pal_log_level_t level) {
  char message[384];
  (void)snprintf(message, sizeof(message),
      "request=conversation stage=%s identity=%llu generation=%llu rc=%d "
      "cancel_source=%d wire_ready=%d committed=%d media_eos=%d "
      "transport_committed=%d audio_rc=%d frames=%zu bytes=%zu",
      stage, (unsigned long long)request->identity,
      (unsigned long long)request->generation, (int)rc,
      atomic_load(&request->cancel_source), atomic_load(&request->wire_ready),
      atomic_load(&request->committed), atomic_load(&request->media_uplink_eos),
      request->transport_committed, atomic_load(&request->audio_result),
      atomic_load(&request->queued_frames), atomic_load(&request->queued_bytes));
  (void)h2_pal_log_write(request->service->client_config.log, level, "gizclaw", message);
}

static h2_pal_result_t
conversation_request_poll(void *user, h2_gizclaw_client_t *client,
                          const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_conversation_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    log_conversation_state(request, "cancel_state", H2_PAL_ERR_CLOSED,
                           H2_PAL_LOG_INFO);
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_DEBUG, "conversation", "poll_cancelled",
        request->identity, H2_PAL_ERR_CLOSED, 0, request->queued_frames,
        request->queued_bytes);
    conversation_request_close(request);
    /* The decoder is detached now, so nothing can append to the Track after
     * this watermark; the discard at cancel only muted the earlier tail. */
    h2_gizclaw_service_pcm_discard_downlink_internal(request->service);
    return H2_PAL_ERR_CLOSED;
  }
  const h2_pal_result_t audio_rc = (h2_pal_result_t)atomic_load_explicit(
      &request->audio_result, memory_order_acquire);
  if (audio_rc != H2_PAL_OK) {
    log_conversation_state(request, "audio_failed", audio_rc, H2_PAL_LOG_ERROR);
    conversation_request_close(request);
    return audio_rc;
  }
  if (!atomic_load_explicit(&request->control_ready, memory_order_acquire)) {
    uint64_t now = 0u;
    h2_pal_result_t rc = h2_gizclaw_client_monotonic_ms_internal(client, &now);
    if (rc == H2_PAL_OK &&
        now - request->bos_started_at_ms >= (uint64_t)request->timeout_ms)
      rc = H2_PAL_ERR_TIMEOUT;
    if (rc != H2_PAL_OK) {
      conversation_request_close(request);
      return rc;
    }
    if (request->conversation == NULL) {
      rc = h2_gizclaw_conversation_wire_open_internal(
          client,
          (h2_gizclaw_str_t){request->workspace_name,
                             request->workspace_name_len},
          request->generation, request->timeout_ms, &request->conversation);
    } else if (!request->conversation->bos_sent) {
      rc = send_boundary(request->conversation, false, false, 0u, NULL);
      if (rc == H2_PAL_OK)
        request->conversation->bos_sent = true;
    }
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      return H2_PAL_ERR_WOULD_BLOCK;
    if (rc != H2_PAL_OK) {
      log_conversation_state(request, "input_bos_failed", rc, H2_PAL_LOG_ERROR);
      conversation_request_close(request);
      return rc;
    }
    request->conversation->service_request = request;
    atomic_store_explicit(&request->control_ready, true, memory_order_release);
  }
  if (!atomic_load_explicit(&request->wire_ready, memory_order_acquire) &&
      atomic_load_explicit(&request->queued_bytes, memory_order_acquire) != 0u) {
    uint64_t now = 0u;
    h2_pal_result_t rc = h2_gizclaw_client_monotonic_ms_internal(client, &now);
    if (request->audio_bos_started_at_ms == 0u)
      request->audio_bos_started_at_ms = now;
    if (rc == H2_PAL_OK &&
        now - request->audio_bos_started_at_ms >= (uint64_t)request->timeout_ms)
      rc = H2_PAL_ERR_TIMEOUT;
    if (rc != H2_PAL_OK) {
      log_conversation_state(request, "input_ready_deadline", rc, H2_PAL_LOG_ERROR);
      conversation_request_close(request);
      return rc;
    }
    if (!request->conversation->audio_bos_sent) {
      rc = h2_gizclaw_conversation_wire_begin_audio_internal(request->conversation);
    }
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      return H2_PAL_ERR_WOULD_BLOCK;
    if (rc != H2_PAL_OK) {
      conversation_request_close(request);
      return rc;
    }
    if (!h2_gizclaw_conversation_wire_input_ready_internal(request->conversation)) {
      rc = h2_gizclaw_client_dispatch_event(client, 0, NULL, NULL);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT) {
        conversation_request_close(request);
        return rc;
      }
    }
    if (h2_gizclaw_conversation_wire_input_ready_internal(request->conversation)) {
      h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_INFO,
          "conversation", "input_ready", request->identity, H2_PAL_OK, 0, 0, 0);
      atomic_store_explicit(&request->wire_ready, true, memory_order_release);
    }
  }
  /* Nonempty input can commit only after READY and media drain. Empty input
   * has no audio channel and completes through the encoder EOS directly. */
  if (request->media_attached &&
      atomic_load_explicit(&request->media_uplink_eos, memory_order_acquire) &&
      (atomic_load_explicit(&request->queued_bytes, memory_order_acquire) == 0u ||
       atomic_load_explicit(&request->wire_ready, memory_order_acquire)) &&
      !request->transport_committed) {
    const h2_pal_result_t commit_rc =
        h2_gizclaw_conversation_wire_finish_input_internal(
            request->conversation, 0u);
    if (commit_rc == H2_PAL_ERR_WOULD_BLOCK)
      return commit_rc;
    if (commit_rc != H2_PAL_OK) {
      h2_gizclaw_service_log_request(
          request->service, H2_PAL_LOG_ERROR, "conversation",
          "commit_send_failed", request->identity, commit_rc, 0,
          request->queued_frames, request->queued_bytes);
      conversation_request_close(request);
      return commit_rc;
    }
    request->transport_committed = true;
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_DEBUG, "conversation", "input_committed",
        request->identity, H2_PAL_OK, 0, request->queued_frames,
        request->queued_bytes);
  }
  if (atomic_load_explicit(&request->input_empty, memory_order_acquire)) {
    if (!request->transport_committed)
      return H2_PAL_ERR_WOULD_BLOCK;
    /* A control-only interruption has no reply to await. EOS has already
     * been accepted by the transport; close through normal completion. */
    conversation_request_close(request);
    return H2_PAL_OK;
  }
  if (request->notification_pending) {
    h2_pal_result_t rc = conversation_notification_step(request);
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    if (rc != H2_PAL_OK || request->notification_terminal) {
      conversation_request_close(request);
      return rc != H2_PAL_OK ? rc : request->notification_terminal_result;
    }
    if (request->dispatch_event.kind ==
        H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE)
      conversation_reply_boundary_dispatched(request);
  }
  const bool downlink_eos =
      atomic_load_explicit(&request->downlink_eos, memory_order_acquire);
  /* Before this reply's boundary: the decoder can only set the flag while
   * the reply's frames precede its EOS marker, and the wire poll below is
   * held while a boundary is staged. Text is not ordered against it: the
   * server streams text and audio independently, and holding text back
   * until audio arrives would only delay the display. */
  if (request->on_event != NULL && !request->reply_audio_notified &&
      atomic_load_explicit(&request->reply_audio_started,
                           memory_order_acquire)) {
    request->reply_audio_notified = true;
    request->dispatch_event = (h2_gizclaw_conversation_event_t){
        .kind = H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO_STARTED,
        .generation = request->generation,
    };
    const h2_pal_result_t dispatch_rc =
        conversation_queue_notification(request, false);
    if (dispatch_rc != H2_PAL_OK && dispatch_rc != H2_PAL_ERR_WOULD_BLOCK) {
      conversation_request_close(request);
      return dispatch_rc;
    }
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (request->terminal_waiting_for_audio && downlink_eos) {
    request->terminal_waiting_for_audio = false;
    if (request->reply_interrupted) {
      request->reply_interrupted = false;
      h2_gizclaw_service_pcm_discard_downlink_internal(request->service);
    }
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_DEBUG, "conversation", "terminal_dispatch",
        request->identity, H2_PAL_OK,
        (int)request->pending_terminal_event.kind,
        request->reply_boundary_terminal, request->generation);
    request->dispatch_event = request->pending_terminal_event;
    request->notification_terminal_result =
        request->dispatch_event.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR
            ? H2_PAL_ERR_IO
            : H2_PAL_OK;
    const h2_pal_result_t dispatch_rc = conversation_queue_notification(
        request, request->reply_boundary_terminal);
    if (dispatch_rc == H2_PAL_ERR_WOULD_BLOCK)
      return dispatch_rc;
    if (dispatch_rc != H2_PAL_OK || request->reply_boundary_terminal) {
      conversation_request_close(request);
      return dispatch_rc != H2_PAL_OK ? dispatch_rc
                                      : request->notification_terminal_result;
    }
    conversation_reply_boundary_dispatched(request);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (request->has_pending_downlink_message) {
    const h2_pal_result_t downlink_rc = audio_ring_send(
        &request->opus_downlink, &request->pending_downlink_message);
    if (downlink_rc == H2_PAL_ERR_WOULD_BLOCK)
      return downlink_rc;
    if (downlink_rc != H2_PAL_OK) {
      h2_gizclaw_service_log_request(
          request->service, H2_PAL_LOG_ERROR, "audio/downlink",
          "ring_write_failed", request->identity, downlink_rc, 0,
          atomic_load_explicit(&request->reply_frames, memory_order_relaxed),
          atomic_load_explicit(&request->reply_bytes, memory_order_relaxed));
      conversation_request_close(request);
      return downlink_rc;
    }
    if (request->pending_downlink_message.kind == H2_GIZCLAW_AUDIO_MESSAGE_EOS)
      request->terminal_waiting_for_audio = true;
    request->has_pending_downlink_message = false;
    memset(&request->pending_downlink_message, 0,
           sizeof(request->pending_downlink_message));
  }
  /* Do not overwrite the pending reply boundary with a later round while its
   * accepted PCM is still draining to the Track. RTP keeps its bounded queue. */
  if (request->terminal_waiting_for_audio)
    return H2_PAL_ERR_WOULD_BLOCK;
  h2_gizclaw_conversation_event_t event = {0};
  h2_pal_result_t rc = h2_gizclaw_conversation_wire_poll_internal(
      request->conversation, 0, &event);
  if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_ERROR, "conversation", "poll_failed",
        request->identity, rc, 0,
        atomic_load_explicit(&request->reply_frames, memory_order_relaxed),
        atomic_load_explicit(&request->reply_bytes, memory_order_relaxed));
    conversation_request_close(request);
    return rc;
  }
  if (event.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE)
    return H2_PAL_ERR_WOULD_BLOCK;
  const bool terminal =
      event.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE ||
      event.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR;
  if (terminal) {
    if (event.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR) {
      snprintf(request->terminal_error_code,
               sizeof(request->terminal_error_code), "%s",
               event.error_code != NULL ? event.error_code : "");
      request->terminal_retryable = event.retryable;
      if (request->service->client_config.log != NULL) {
        char message[160];
        snprintf(message, sizeof(message),
                 "remote_error code=%s retryable=%s",
                 request->terminal_error_code,
                 event.retryable ? "true" : "false");
        (void)h2_pal_log_write(request->service->client_config.log,
                              H2_PAL_LOG_ERROR, "gizclaw/conversation", message);
      }
    }
    request->reply_boundary_terminal =
        event.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR ||
        request->transport_committed;
    request->reply_interrupted =
        h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
            request->conversation);
    /* The user spoke over this reply: whatever the Track still holds of it is
     * stale. Frames the decoder has not consumed yet drain behind the EOS
     * marker and are discarded again when the boundary is dispatched. */
    if (request->reply_interrupted)
      h2_gizclaw_service_pcm_discard_downlink_internal(request->service);
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_DEBUG, "conversation", "terminal_staged",
        request->identity, H2_PAL_OK,
        (int)event.kind * 10 + (request->reply_boundary_terminal ? 1 : 0) +
            (request->reply_interrupted ? 100 : 0),
        request->on_event != NULL, event.generation);
    request->pending_terminal_event = event;
    request->pending_downlink_message =
        (h2_gizclaw_conversation_request_message_t){
            .kind = H2_GIZCLAW_AUDIO_MESSAGE_EOS};
    request->has_pending_downlink_message = true;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  request->dispatch_event = event;
  rc = conversation_queue_notification(request, false);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return rc;
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(
        request->service, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        "conversation", "event_dispatch_failed", request->identity, rc,
        (int)event.kind,
        atomic_load_explicit(&request->reply_frames, memory_order_relaxed),
        atomic_load_explicit(&request->reply_bytes, memory_order_relaxed));
  }
  if (rc != H2_PAL_OK)
    conversation_request_close(request);
  if (rc != H2_PAL_OK)
    return rc;
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t
conversation_request_start(void *user, h2_gizclaw_client_t *client,
                           const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_conversation_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    log_conversation_state(request, "cancel_state", H2_PAL_ERR_CLOSED,
                           H2_PAL_LOG_INFO);
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_DEBUG, "conversation", "start_cancelled",
        request->identity, H2_PAL_ERR_CLOSED, 0, request->queued_frames,
        request->queued_bytes);
    return H2_PAL_ERR_CLOSED;
  }
  const h2_pal_result_t rc = h2_gizclaw_client_monotonic_ms_internal(
      client, &request->bos_started_at_ms);
  if (rc != H2_PAL_OK) {
    log_conversation_state(request, "start_clock_failed", rc, H2_PAL_LOG_ERROR);
    conversation_request_close(request);
    return rc;
  }
  return conversation_request_poll(user, client, cancel_token);
}

static void
conversation_request_complete(void *user, h2_gizclaw_operation_t *operation,
                              const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  h2_gizclaw_conversation_request_t *request = user;
  request->operation_result = *result;
  memcpy(request->operation_result.error_code, request->terminal_error_code,
         sizeof(request->operation_result.error_code));
  request->operation_result.retryable = request->terminal_retryable;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  h2_gizclaw_service_log_request(
      request->service,
      result->result == H2_PAL_OK ||
              result->terminal_kind == H2_GIZCLAW_OPERATION_CANCELED
          ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
      "conversation", "completed", request->identity, result->result,
      (int)result->terminal_kind,
      request->queued_frames, request->queued_bytes);
  h2_gizclaw_service_log_request(
      request->service, H2_PAL_LOG_INFO, "conversation", "reply_summary",
      request->identity, result->result, 0,
      atomic_load_explicit(&request->reply_frames, memory_order_relaxed),
      atomic_load_explicit(&request->reply_bytes, memory_order_relaxed));
  request->completion(request->user, request);
}

static void record_audio_request(h2_gizclaw_audio_log_t *log,
                                  const char *stage, uint64_t identity,
                                  h2_pal_result_t rc) {
  char *message = h2_gizclaw_audio_log_append_internal(
      log, rc == H2_PAL_OK ? H2_PAL_LOG_DEBUG : H2_PAL_LOG_ERROR);
  if (message != NULL)
    (void)snprintf(message, H2_PAL_LOG_MESSAGE_MAX,
                   "request=conversation stage=%s identity=%llu rc=%d",
                   stage, (unsigned long long)identity, (int)rc);
}

static h2_pal_result_t conversation_generation_start(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, uint64_t generation, int timeout_ms,
    conversation_generation_event_fn on_event,
    conversation_generation_completion_fn completion, void *user,
    h2_gizclaw_conversation_request_t **out_request,
    h2_gizclaw_audio_log_t *log) {
  if (service == NULL || !valid_workspace(workspace_name) || timeout_ms <= 0 ||
      completion == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_request = NULL;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_conversation_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  /* 20 ms at 16 kHz until the first real packet says otherwise. */
  request->plc_frame_samples = 320;
  request->service = service;
  request->identity = identity;
  request->on_event = on_event;
  request->completion = completion;
  request->user = user;
  request->generation = generation;
  request->timeout_ms = timeout_ms;
  request->workspace_name_len = workspace_name.len;
  memcpy(request->workspace_name, workspace_name.data, workspace_name.len);
  request->workspace_name[workspace_name.len] = '\0';
  atomic_init(&request->committed, false);
  atomic_init(&request->reply_audio_started, false);
  atomic_init(&request->reply_media_open, false);
  atomic_init(&request->discard_frames_before, 0u);
  atomic_init(&request->discard_downlink_pending, false);
  request->decoded_frames = 0u;
  atomic_init(&request->playback_canceled, false);
  atomic_init(&request->input_empty, false);
  atomic_init(&request->wire_ready, false);
  atomic_init(&request->control_ready, false);
  atomic_init(&request->notification_done, false);
  atomic_init(&request->notification_suppressed, false);
  atomic_init(&request->terminal, false);
  atomic_init(&request->downlink_eos, false);
  atomic_init(&request->media_uplink_eos, false);
  atomic_init(&request->audio_result, H2_PAL_OK);
  atomic_init(&request->cancel_source, H2_GIZCLAW_CANCEL_UNSPECIFIED);
  atomic_init(&request->queued_frames, 0u);
  atomic_init(&request->queued_bytes, 0u);
  atomic_init(&request->pcm_write_failures, 0u);
  atomic_init(&request->reply_frames, 0u);
  atomic_init(&request->reply_bytes, 0u);
  h2_pal_mutex_config_t input_config = {.name = "$gizclaw/conversation-input",
                                        .allocator = allocator};
  const char *failure_stage = "create_input_mutex";
  h2_pal_result_t rc = h2_pal_mutex_create(service->config.sync, &input_config,
                                           &request->input_mutex);
  if (rc == H2_PAL_OK) {
    failure_stage = "create_uplink_ring";
    rc = audio_ring_init(&request->opus_uplink, service,
                         sizeof(h2_gizclaw_conversation_request_message_t),
                         H2_GIZCLAW_CONVERSATION_OPUS_UPLINK_RING_ITEMS);
  }
  if (rc == H2_PAL_OK) {
    failure_stage = "create_downlink_ring";
    rc = audio_ring_init(&request->opus_downlink, service,
                         sizeof(h2_gizclaw_conversation_request_message_t),
                         H2_GIZCLAW_CONVERSATION_OPUS_DOWNLINK_RING_ITEMS);
  }
  /* Snapshot before attaching; the uplink task alone discards stale PCM. */
  if (rc == H2_PAL_OK) {
    failure_stage = "create_pcm_input";
    rc = h2_gizclaw_service_pcm_input_internal(
        service, &request->input, H2_GIZCLAW_PCM_INPUT_START, NULL, 0u, NULL);
  }
  /* Reserve the route before admission. Service audio workers wait for BOS. */
  if (rc == H2_PAL_OK) {
    failure_stage = "create_media_attach";
    rc = h2_gizclaw_conversation_media_attach(service, request);
  }
  if (rc == H2_PAL_OK) {
    failure_stage = "create_submit";
    rc = h2_gizclaw_service_submit_async_internal(
        service, identity, conversation_request_start,
        conversation_request_poll, conversation_request_complete, request,
        &request->operation);
  }
  if (rc != H2_PAL_OK) {
    record_audio_request(log, failure_stage, identity, rc);
    h2_gizclaw_conversation_media_detach(request);
    audio_ring_close(&request->opus_uplink);
    audio_ring_close(&request->opus_downlink);
    if (request->input_mutex != NULL)
      (void)h2_pal_mutex_destroy(service->config.sync, request->input_mutex);
    audio_ring_deinit(&request->opus_uplink);
    audio_ring_deinit(&request->opus_downlink);
    h2_gizclaw_pcm_input_deinit(&request->input);
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  record_audio_request(log, "created", identity, H2_PAL_OK);
  *out_request = request;
  return H2_PAL_OK;
}

static h2_pal_result_t conversation_generation_finish_input(
    h2_gizclaw_conversation_request_t *request, h2_gizclaw_audio_log_t *log) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(request->service->config.sync, request->input_mutex);
  if (rc != H2_PAL_OK) {
    record_audio_request(log, "commit_input_lock", request->identity, rc);
    return rc;
  }
  if (atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    rc = H2_PAL_ERR_CLOSED;
  }
  if (rc == H2_PAL_OK && !atomic_load(&request->committed)) {
    rc = h2_gizclaw_service_pcm_input_internal(
        request->service, &request->input, H2_GIZCLAW_PCM_INPUT_END, NULL, 0u,
        NULL);
    if (rc == H2_PAL_OK) {
      atomic_store_explicit(&request->input_empty, request->input.empty,
                            memory_order_release);
      if (request->input.empty) {
        atomic_store_explicit(&request->playback_canceled, true,
                              memory_order_release);
        atomic_store_explicit(&request->reply_media_open, false,
                              memory_order_release);
        h2_gizclaw_service_pcm_discard_downlink_internal(request->service);
      }
      atomic_store_explicit(&request->committed, true, memory_order_release);
    }
  }
  (void)h2_pal_mutex_unlock(request->service->config.sync,
                            request->input_mutex);
  record_audio_request(log, "commit", request->identity, rc);
  return rc;
}

static void
conversation_generation_destroy(h2_gizclaw_conversation_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  h2_gizclaw_conversation_media_detach(request);
  audio_ring_close(&request->opus_uplink);
  audio_ring_close(&request->opus_downlink);
  (void)h2_pal_mutex_destroy(request->service->config.sync,
                             request->input_mutex);
  h2_pal_mem_free(request->service->client_config.allocator, request->encoder);
  h2_pal_mem_free(request->service->client_config.allocator, request->decoder);
  audio_ring_deinit(&request->opus_uplink);
  audio_ring_deinit(&request->opus_downlink);
  h2_gizclaw_pcm_input_deinit(&request->input);
  h2_pal_mem_free(request->service->config.client_config->allocator, request);
}

static h2_pal_result_t
service_conversation_event(void *user,
                           h2_gizclaw_conversation_request_t *request,
                           const h2_gizclaw_conversation_event_t *event) {
  (void)request;
  h2_gizclaw_conversation_t *conversation = user;
  if (conversation == NULL || !conversation->service_mode)
    return H2_PAL_ERR_INVALID_STATE;
  return conversation->callback == NULL
             ? H2_PAL_OK
             : conversation->callback(conversation->callback_user, conversation,
                                      event);
}

/* audio_mutex protects the logical route and request lifetime. */
static void record_audio_control(h2_gizclaw_service_t *service,
                              h2_gizclaw_conversation_t *conversation,
                              const char *stage, h2_pal_result_t rc,
                              h2_pal_log_level_t success_level,
                              h2_gizclaw_audio_log_t *log) {
  if (service->client_config.log == NULL)
    return;
  const h2_gizclaw_conversation_request_t *request =
      conversation != NULL ? conversation->service_request : NULL;
  char *message = h2_gizclaw_audio_log_append_internal(
      log, rc == H2_PAL_OK ? success_level : H2_PAL_LOG_ERROR);
  if (message == NULL)
    return;
  (void)snprintf(message, H2_PAL_LOG_MESSAGE_MAX,
                 "service=%p conversation=%p stage=%s rc=%d identity=%llu "
                 "generation=%llu next=%llu request=%d input_ended=%d "
                 "audio_ended=%d cancel_source=%d",
                 (void *)service, (void *)conversation, stage, (int)rc,
                 (unsigned long long)(request != NULL ? request->identity : 0u),
                 (unsigned long long)(request != NULL ? request->generation : 0u),
                 (unsigned long long)(conversation != NULL
                                          ? conversation->next_generation : 0u),
                 request != NULL, conversation != NULL && conversation->input_ended,
                 service->audio_ended,
                 request != NULL ? atomic_load(&request->cancel_source) : 0);
}

static void
service_conversation_complete(void *user,
                              h2_gizclaw_conversation_request_t *request) {
  h2_gizclaw_audio_log_t logs = {0};
  h2_gizclaw_conversation_t *conversation = user;
  h2_gizclaw_service_t *service = request->service;
  (void)h2_pal_mutex_lock(service->config.sync, service->audio_mutex);
  if (conversation == NULL || conversation->service_request != request) {
    conversation_generation_destroy(request);
    (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
    return;
  }
  record_audio_control(service, conversation, "completion_release", H2_PAL_OK,
                    H2_PAL_LOG_DEBUG, &logs);
  const h2_gizclaw_operation_result_t result_copy = request->operation_result;
  h2_gizclaw_conversation_completion_fn completion = conversation->completion;
  void *callback_user = conversation->callback_user;
  conversation->service_request = NULL;
  conversation_generation_destroy(request);
  (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
  if (completion != NULL)
    completion(callback_user, conversation, &result_copy);
  h2_gizclaw_service_flush_audio_log_internal(service, &logs);
}

h2_pal_result_t h2_gizclaw_conversation_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t workspace,
    h2_gizclaw_conversation_callback_fn callback,
    h2_gizclaw_conversation_completion_fn completion, void *user,
    h2_gizclaw_conversation_t **out_conversation) {
  if (service == NULL || !valid_workspace(workspace) ||
      out_conversation == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_conversation = NULL;
  h2_pal_result_t lock_rc =
      h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  if (service->stopping || service->stopped) {
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    return H2_PAL_ERR_CLOSED;
  }
  if (service->audio_conversation != NULL) {
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    return H2_PAL_ERR_BUSY;
  }
  h2_gizclaw_conversation_t *conversation = h2_pal_mem_alloc(
      service->config.client_config->allocator, sizeof(*conversation));
  if (conversation == NULL) {
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(conversation, 0, sizeof(*conversation));
  conversation->service = service;
  conversation->allocator = service->config.client_config->allocator;
  conversation->callback = callback;
  conversation->completion = completion;
  conversation->callback_user = user;
  conversation->next_generation = 1u;
  conversation->service_mode = 1;
  memcpy(conversation->workspace_name, workspace.data, workspace.len);
  conversation->workspace_name[workspace.len] = '\0';
  service->audio_conversation = conversation;
  ++service->request_reference_count;
  *out_conversation = conversation;
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t
conversation_audio_start(h2_gizclaw_conversation_t *conversation,
                         h2_gizclaw_audio_log_t *log) {
  if (conversation == NULL || !conversation->service_mode)
    return H2_PAL_ERR_INVALID_ARG;
  if (conversation->service_request != NULL) {
    record_audio_request(log, "start_has_request",
                         conversation->service_request->identity,
                         H2_PAL_ERR_INVALID_STATE);
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (!h2_gizclaw_service_pcm_readable_internal(conversation->service)) {
    record_audio_request(log, "start_no_readable_track", 0u,
                         H2_PAL_ERR_INVALID_STATE);
    return H2_PAL_ERR_INVALID_STATE;
  }
  const uint64_t generation = conversation->next_generation++;
  const int timeout_ms =
      conversation->service->client_config.connect_timeout_ms > 0
          ? conversation->service->client_config.connect_timeout_ms
          : 30000;
  h2_gizclaw_conversation_request_t *request = NULL;
  const h2_pal_result_t rc = conversation_generation_start(
      conversation->service, generation,
      (h2_gizclaw_str_t){.data = conversation->workspace_name,
                         .len = strlen(conversation->workspace_name)},
      generation, timeout_ms,
      conversation->callback != NULL ? service_conversation_event : NULL,
      service_conversation_complete, conversation, &request, log);
  if (rc == H2_PAL_OK) {
    conversation->service_request = request;
    conversation->input_ended = false;
  }
  return rc;
}

static h2_pal_result_t
conversation_audio_end(h2_gizclaw_conversation_t *conversation,
                       h2_gizclaw_audio_log_t *log) {
  if (conversation == NULL || !conversation->service_mode)
    return H2_PAL_ERR_INVALID_ARG;
  if (conversation->input_ended)
    return H2_PAL_OK;
  if (conversation->service_request == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  const h2_pal_result_t rc =
      conversation_generation_finish_input(conversation->service_request, log);
  if (rc == H2_PAL_OK)
    conversation->input_ended = true;
  return rc;
}

/* Control calls serialize route selection with admission and destruction.
 * PCM copying/encoding remains on the sole uplink consumer. */
h2_pal_result_t h2_gizclaw_service_audio_control_internal(
    h2_gizclaw_service_t *service, bool start, h2_gizclaw_audio_log_t *log,
    bool *out_empty) {
  if (out_empty != NULL)
    *out_empty = false;
  if (start)
    h2_gizclaw_service_pcm_discard_downlink_internal(service);
  if (service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(service->config.sync, service->audio_mutex);
  if (rc != H2_PAL_OK) {
    record_audio_request(log, start ? "start_audio_lock" : "end_audio_lock", 0u, rc);
    return rc;
  }
  rc = h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (rc != H2_PAL_OK) {
    record_audio_request(log, start ? "start_service_lock" : "end_service_lock", 0u, rc);
    (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
    return rc;
  }
  void *speech = atomic_load(&service->speech_request);
  h2_gizclaw_conversation_t *conversation = service->audio_conversation;
  bool closed = service->stopping || service->stopped;
  bool started = service->started;
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  if (closed)
    rc = H2_PAL_ERR_CLOSED;
  else if (!started)
    rc = H2_PAL_ERR_INVALID_STATE;
  else if (!start && service->audio_ended)
    rc = H2_PAL_OK;
  else if (speech != NULL)
    rc = start ? h2_gizclaw_speech_audio_start_internal(speech)
               : h2_gizclaw_speech_audio_end_internal(speech);
  else if (conversation != NULL)
    rc = start ? conversation_audio_start(conversation, log)
               : conversation_audio_end(conversation, log);
  else
    rc = H2_PAL_ERR_INVALID_STATE;
  record_audio_control(service, conversation,
                    start ? "service_audio_start" : "service_audio_end", rc,
                    H2_PAL_LOG_INFO, log);
  if (rc == H2_PAL_OK) {
    service->audio_ended = !start;
    if (!start && out_empty != NULL && conversation != NULL &&
        conversation->service_request != NULL)
      *out_empty = atomic_load_explicit(
          &conversation->service_request->input_empty, memory_order_acquire);
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
  return rc;
}

static h2_pal_result_t service_audio_control(h2_gizclaw_service_t *service,
                                             bool start) {
  h2_gizclaw_audio_log_t logs = {0};
  h2_pal_result_t rc =
      h2_gizclaw_service_audio_control_internal(service, start, &logs, NULL);
  h2_gizclaw_service_flush_audio_log_internal(service, &logs);
  return rc;
}

h2_pal_result_t h2_gizclaw_service_audio_start(h2_gizclaw_service_t *service) {
  return service_audio_control(service, true);
}

h2_pal_result_t h2_gizclaw_service_audio_end(h2_gizclaw_service_t *service) {
  return service_audio_control(service, false);
}

h2_pal_result_t
h2_gizclaw_conversation_cancel_internal(h2_gizclaw_conversation_t *conversation,
                                        h2_gizclaw_audio_log_t *log,
                                        int source) {
  if (conversation == NULL || !conversation->service_mode)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_service_t *service = conversation->service;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(service->config.sync, service->audio_mutex);
  if (rc != H2_PAL_OK) {
    record_audio_request(log, "cancel_audio_lock", 0u, rc);
    return rc;
  }
  if (conversation->service_request != NULL) {
    int expected = H2_GIZCLAW_CANCEL_UNSPECIFIED;
    (void)atomic_compare_exchange_strong(
        &conversation->service_request->cancel_source, &expected, (int)source);
    h2_gizclaw_conversation_request_t *request = conversation->service_request;
    rc = h2_pal_mutex_lock(service->config.sync, request->input_mutex);
    if (rc != H2_PAL_OK) {
      (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
      return rc;
    }
    atomic_store_explicit(&request->playback_canceled, true, memory_order_release);
    atomic_store_explicit(&request->reply_media_open, false, memory_order_release);
    h2_gizclaw_service_pcm_discard_downlink_internal(service);
    (void)h2_pal_mutex_unlock(service->config.sync, request->input_mutex);
    rc = h2_gizclaw_operation_cancel(request->operation);
    record_audio_control(service, conversation, "cancel_requested", rc,
                      H2_PAL_LOG_DEBUG, log);
    h2_gizclaw_service_pcm_discard_downlink_internal(service);
  } else {
    record_audio_control(service, conversation, "cancel_no_request", rc,
                         H2_PAL_LOG_DEBUG, log);
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
  return rc;
}

h2_pal_result_t
h2_gizclaw_conversation_cancel(h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL || !conversation->service_mode)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_service_t *service = conversation->service;
  h2_gizclaw_audio_log_t logs = {0};
  h2_pal_result_t rc = h2_gizclaw_conversation_cancel_internal(
      conversation, &logs, H2_GIZCLAW_CANCEL_API);
  h2_gizclaw_service_flush_audio_log_internal(service, &logs);
  return rc;
}

void h2_gizclaw_conversation_release(h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL || !conversation->service_mode)
    return;
  h2_gizclaw_service_t *service = conversation->service;
  if (h2_pal_mutex_lock(service->config.sync, service->audio_mutex) !=
      H2_PAL_OK)
    return;
  if (conversation->service_request == NULL) {
    (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
    service->audio_conversation = NULL;
    --service->request_reference_count;
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    h2_pal_mem_free(conversation->allocator, conversation);
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
}

/* The Session has drained the previous generation before changing routes. */
h2_pal_result_t h2_gizclaw_conversation_retarget_internal(
    h2_gizclaw_conversation_t *conversation, const char *workspace) {
  if (conversation == NULL || workspace == NULL || !conversation->service_mode ||
      !valid_workspace((h2_gizclaw_str_t){workspace, strlen(workspace)}))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_service_t *service = conversation->service;
  h2_pal_result_t rc = h2_pal_mutex_lock(service->config.sync, service->audio_mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (conversation->service_request != NULL) {
    rc = H2_PAL_ERR_BUSY;
  } else {
    memcpy(conversation->workspace_name, workspace, strlen(workspace) + 1u);
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
  return rc;
}
