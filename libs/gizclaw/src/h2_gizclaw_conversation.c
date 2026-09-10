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
  h2_pal_mutex_t *input_mutex;
  OpusEncoder *encoder;
  uint8_t capture[H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES];
  size_t capture_len;
  h2_gizclaw_pcm_input_t input; /* Protected by input_mutex. */
  h2_gizclaw_conversation_request_message_t encoded;
  bool encoded_pending;
  bool encoder_eos;
  atomic_bool input_empty;
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
  h2_gizclaw_conversation_event_t dispatch_event;
  char dispatch_text[H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES + 1u];
  char dispatch_error[65];
  bool notification_pending, notification_queued, notification_terminal;
  atomic_bool notification_done, notification_suppressed;
  h2_pal_result_t notification_result, notification_terminal_result;
  uint64_t identity;
  atomic_size_t queued_frames;
  atomic_size_t queued_bytes;
  h2_gizclaw_operation_result_t operation_result;
  char terminal_error_code[65];
  bool terminal_retryable;
  atomic_bool committed;
  atomic_bool terminal;
  atomic_bool media_uplink_eos;
  atomic_int audio_result;
  atomic_int cancel_source;
  bool media_attached;
  bool transport_committed;
};

/* Downstream audio of the Conversation route. It is not part of any input
 * request: whatever the server sends is decoded into the Track and played,
 * whatever the stream ID, BOS or EOS. The only local rule is that releasing
 * push-to-talk clears what is buffered at that moment. */
struct h2_gizclaw_conversation_downlink {
  h2_gizclaw_audio_ring_t opus;
  /* Held by the decoder for one step and by flush; flush acts as the ring's
   * consumer while it holds this lock. */
  h2_pal_mutex_t *decode_lock;
  OpusDecoder *decoder;
  int16_t decoded[H2_GIZCLAW_CONVERSATION_DECODE_MAX_SAMPLES];
  size_t decoded_len, decoded_offset;
  /* Samples the last real packet decoded to; a loss marker is concealed for
   * exactly that duration instead of the decoder's maximum frame. */
  int plc_frame_samples;
  atomic_size_t frames;
  atomic_size_t bytes;
  /* Chunks written to the Track: the Session's sign that sound arrived. */
  atomic_size_t pcm_writes;
};

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
  uint64_t sequence;
  bool bos_sent;
  bool audio_bos_requested;
  bool audio_bos_sent;
  bool audio_eos_sent;
  bool input_ready;
  bool input_rejected;
  bool committed;
  bool canceled;
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

/* The downlink exists while a Conversation route is configured. out_track
 * reports whether the Track's downlink is free for conversation audio:
 * another owner (audio play, speech) keeps conversation audio out of it. */
static h2_gizclaw_conversation_downlink_t *
downlink_acquire_any(h2_gizclaw_service_t *service, bool *out_track) {
  if (service == NULL ||
      h2_pal_mutex_lock(service->config.sync, service->mutex) != H2_PAL_OK)
    return NULL;
  h2_gizclaw_conversation_downlink_t *downlink = service->conversation_downlink;
  if (downlink != NULL)
    ++service->downlink_refs;
  if (out_track != NULL)
    *out_track = service->audio_play == NULL &&
                 atomic_load(&service->speech_request) == NULL;
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  return downlink;
}

static void downlink_release(h2_gizclaw_service_t *service);

/* A downlink whose audio may reach the Track now. */
static h2_gizclaw_conversation_downlink_t *
downlink_acquire(h2_gizclaw_service_t *service) {
  bool track = false;
  h2_gizclaw_conversation_downlink_t *downlink =
      downlink_acquire_any(service, &track);
  if (downlink != NULL && !track) {
    downlink_release(service);
    return NULL;
  }
  return downlink;
}

static void downlink_release(h2_gizclaw_service_t *service) {
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (--service->downlink_refs == 0u)
    (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

h2_pal_result_t
h2_gizclaw_service_media_write_opus(h2_gizclaw_service_t *service,
                                    const uint8_t *opus, size_t opus_len) {
  if (service == NULL || (opus == NULL && opus_len != 0u) ||
      opus_len > H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_conversation_downlink_t *downlink = downlink_acquire(service);
  if (downlink == NULL)
    return H2_PAL_OK;
  h2_gizclaw_conversation_request_message_t message = {
      .kind = H2_GIZCLAW_AUDIO_MESSAGE_OPUS, .len = opus_len};
  if (opus_len != 0u)
    memcpy(message.data, opus, opus_len);
  h2_pal_result_t rc = audio_ring_send(&downlink->opus, &message);
  if (rc == H2_PAL_OK) {
    atomic_fetch_add_explicit(&downlink->frames, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&downlink->bytes, opus_len, memory_order_relaxed);
  } else if (rc == H2_PAL_ERR_CLOSED) {
    rc = H2_PAL_OK;
  }
  downlink_release(service);
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

/* One decoded chunk into the Track. Called with decode_lock held. */
static h2_pal_result_t
downlink_decode_step(h2_gizclaw_service_t *service,
                     h2_gizclaw_conversation_downlink_t *downlink) {
  if (downlink->decoder == NULL) {
    int size = opus_decoder_get_size(1);
    downlink->decoder =
        size > 0 ? h2_pal_mem_alloc(service->client_config.allocator,
                                    (size_t)size)
                 : NULL;
    if (downlink->decoder == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    if (opus_decoder_init(downlink->decoder, 16000, 1) != OPUS_OK) {
      h2_pal_mem_free(service->client_config.allocator, downlink->decoder);
      downlink->decoder = NULL;
      return H2_PAL_ERR_FORMAT;
    }
  }
  if (downlink->decoded_offset == downlink->decoded_len) {
    h2_gizclaw_conversation_request_message_t input;
    h2_pal_result_t rc = audio_ring_recv(&downlink->opus, &input);
    if (rc != H2_PAL_OK)
      return rc;
    if (input.kind != H2_GIZCLAW_AUDIO_MESSAGE_OPUS)
      return H2_PAL_OK;
    /* A zero-length item is the transport's RTP loss marker: ask Opus for
     * packet loss concealment over one packet's worth of samples. A packet
     * that does not decode is dropped; the stream goes on. */
    int samples = opus_decode(downlink->decoder, input.len ? input.data : NULL,
                              (opus_int32)input.len, downlink->decoded,
                              input.len != 0u
                                  ? (int)H2_GIZCLAW_CONVERSATION_DECODE_MAX_SAMPLES
                                  : downlink->plc_frame_samples,
                              0);
    if (samples <= 0)
      return H2_PAL_OK;
    if (input.len != 0u)
      downlink->plc_frame_samples = samples;
    downlink->decoded_len = (size_t)samples * 2;
    downlink->decoded_offset = 0;
  }
  size_t len = downlink->decoded_len - downlink->decoded_offset;
  if (len > H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES)
    len = H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES;
  uint8_t pcm[H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES];
  for (size_t i = 0; i < len / 2; ++i) {
    uint16_t sample =
        (uint16_t)downlink->decoded[downlink->decoded_offset / 2 + i];
    pcm[i * 2] = (uint8_t)sample;
    pcm[i * 2 + 1] = (uint8_t)(sample >> 8);
  }
  const h2_pal_result_t rc =
      h2_gizclaw_service_pcm_write_internal(service, pcm, len);
  if (rc != H2_PAL_OK)
    return rc;
  downlink->decoded_offset += len;
  atomic_fetch_add_explicit(&downlink->pcm_writes, 1u, memory_order_release);
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
  bool track = false;
  h2_gizclaw_conversation_downlink_t *downlink =
      downlink_acquire_any(service, &track);
  if (downlink == NULL)
    return;
  /* One decoded chunk per wake caps this stage at roughly real time, so any
   * wake lost to scheduling leaves the Opus ring fuller for good and the PCM
   * Track intermittently dry. While the ring has a backlog and the Track has
   * room, keep decoding; the Track's own depth bounds the burst and its
   * WOULD_BLOCK ends it, so the speaker still paces playback. */
  if (track && h2_pal_mutex_lock(service->config.sync, downlink->decode_lock) ==
                   H2_PAL_OK) {
    for (unsigned int chunk = 0u;
         chunk < H2_GIZCLAW_CONVERSATION_DECODE_BURST_CHUNKS; ++chunk) {
        if (downlink_decode_step(service, downlink) != H2_PAL_OK)
        break;
    }
    (void)h2_pal_mutex_unlock(service->config.sync, downlink->decode_lock);
  }
  downlink_release(service);
}

/* Releasing push-to-talk: drop everything buffered so far, queued Opus,
 * the half-decoded packet and the Track's unplayed PCM. Later audio plays. */
void h2_gizclaw_conversation_downlink_flush_internal(
    h2_gizclaw_service_t *service) {
  bool track = false;
  h2_gizclaw_conversation_downlink_t *downlink =
      downlink_acquire_any(service, &track);
  if (downlink == NULL)
    return;
  if (h2_pal_mutex_lock(service->config.sync, downlink->decode_lock) ==
      H2_PAL_OK) {
    atomic_store_explicit(
        &downlink->opus.read_index,
        atomic_load_explicit(&downlink->opus.write_index, memory_order_acquire),
        memory_order_release);
    downlink->decoded_len = 0u;
    downlink->decoded_offset = 0u;
    if (downlink->decoder != NULL)
      (void)opus_decoder_ctl(downlink->decoder, OPUS_RESET_STATE);
    if (track)
      h2_gizclaw_service_pcm_discard_downlink_internal(service);
    (void)h2_pal_mutex_unlock(service->config.sync, downlink->decode_lock);
  }
  downlink_release(service);
}

size_t h2_gizclaw_conversation_downlink_writes_internal(
    h2_gizclaw_service_t *service) {
  h2_gizclaw_conversation_downlink_t *downlink =
      downlink_acquire_any(service, NULL);
  if (downlink == NULL)
    return 0u;
  const size_t writes =
      atomic_load_explicit(&downlink->pcm_writes, memory_order_acquire);
  downlink_release(service);
  return writes;
}

static h2_pal_result_t
downlink_create(h2_gizclaw_service_t *service,
                h2_gizclaw_conversation_downlink_t **out_downlink) {
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_conversation_downlink_t *downlink =
      h2_pal_mem_alloc(allocator, sizeof(*downlink));
  if (downlink == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(downlink, 0, sizeof(*downlink));
  /* 20 ms at 16 kHz until the first real packet says otherwise. */
  downlink->plc_frame_samples = 320;
  atomic_init(&downlink->frames, 0u);
  atomic_init(&downlink->bytes, 0u);
  atomic_init(&downlink->pcm_writes, 0u);
  const h2_pal_mutex_config_t lock_config = {
      .name = "$gizclaw/downlink", .allocator = allocator};
  h2_pal_result_t rc = h2_pal_mutex_create(service->config.sync, &lock_config,
                                           &downlink->decode_lock);
  if (rc == H2_PAL_OK)
    rc = audio_ring_init(&downlink->opus, service,
                         sizeof(h2_gizclaw_conversation_request_message_t),
                         H2_GIZCLAW_CONVERSATION_OPUS_DOWNLINK_RING_ITEMS);
  if (rc != H2_PAL_OK) {
    if (downlink->decode_lock != NULL)
      (void)h2_pal_mutex_destroy(service->config.sync, downlink->decode_lock);
    h2_pal_mem_free(allocator, downlink);
    return rc;
  }
  *out_downlink = downlink;
  return H2_PAL_OK;
}

void h2_gizclaw_conversation_downlink_destroy_internal(
    h2_gizclaw_service_t *service) {
  h2_gizclaw_conversation_downlink_t *downlink = service->conversation_downlink;
  service->conversation_downlink = NULL;
  if (downlink == NULL)
    return;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  audio_ring_close(&downlink->opus);
  audio_ring_deinit(&downlink->opus);
  (void)h2_pal_mutex_destroy(service->config.sync, downlink->decode_lock);
  h2_pal_mem_free(allocator, downlink->decoder);
  h2_pal_mem_free(allocator, downlink);
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

/* Our own input: READY, the server's echo of our boundaries, or its
 * rejection. Stream IDs of everything the server sends us are its own and
 * mean nothing locally. */
static bool event_names_our_input(const h2_gizclaw_conversation_t *conversation,
                                  const gzc_peer_event_t *event) {
  const char *id = peer_event_stream_id(event);
  const char *label = peer_event_label(event);
  return id != NULL && conversation->stream_id[0] != '\0' &&
         strcmp(id, conversation->stream_id) == 0 &&
         (label == NULL || label[0] == '\0' ||
          strcmp(label, H2_GIZCLAW_CONVERSATION_INPUT_LABEL) == 0);
}

/* The Conversation looks at its own input and forwards text. Downstream
 * boundaries (BOS, EOS, stream IDs) carry nothing it acts on: downstream
 * audio plays through the Track whatever the server does with its streams. */
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
  return !conversation->canceled;
}

bool h2_gizclaw_conversation_accepts_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event) {
  return accepts_peer_event(conversation, event);
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
  const char *label = peer_event_label(event);
  const char *id = peer_event_stream_id(event);
  int kind = -1;
  if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS)
    kind = (int)event->payload.bos.kind;
  else if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS)
    kind = (int)event->payload.eos.kind;
  (void)snprintf(
      out, cap,
      "generation=%llu input=%.24s ready=%d committed=%d canceled=%d "
      "label=%.12s id=%.40s kind=%d error=%d",
      (unsigned long long)conversation->generation, conversation->stream_id,
      conversation->input_ready, conversation->committed, conversation->canceled,
      label != NULL ? label : "-", id != NULL ? id : "-", kind,
      event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS
          ? (int)event->payload.eos.has_error : 0);
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
      event->payload.eos.has_error && event_names_our_input(conversation, event))
    conversation->input_rejected = true;
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
    return H2_PAL_OK;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS:
    /* The server refusing our own input is the only error here. The end of
     * anything it sends us, with or without an error code, is not. */
    if (!event.payload.eos.has_error ||
        !event_names_our_input(conversation, &event)) {
      out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_NONE;
      return H2_PAL_OK;
    }
    (void)snprintf(conversation->error_code, sizeof(conversation->error_code),
                   "%s", event.payload.eos.error.code);
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_ERROR;
    out_event->error_code = conversation->error_code;
    out_event->retryable = event.payload.eos.error.retryable;
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
  if (request->conversation == NULL)
    return;
  h2_gizclaw_conversation_wire_destroy_internal(request->conversation);
  request->conversation = NULL;
}

#define conversation_request_close(request)                                    \
  conversation_request_close_at((request), __LINE__)

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
  if (request->notification_pending) {
    h2_pal_result_t rc = conversation_notification_step(request);
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    if (rc != H2_PAL_OK || request->notification_terminal) {
      conversation_request_close(request);
      return rc != H2_PAL_OK ? rc : request->notification_terminal_result;
    }
  }
  /* The input end is on the wire: this request is done. It never waits for
   * the server; downstream audio plays through the downlink on its own. */
  if (request->transport_committed) {
    conversation_request_close(request);
    return H2_PAL_OK;
  }
  h2_gizclaw_conversation_event_t event = {0};
  h2_pal_result_t rc = h2_gizclaw_conversation_wire_poll_internal(
      request->conversation, 0, &event);
  if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_ERROR, "conversation", "poll_failed",
        request->identity, rc, 0, request->queued_frames,
        request->queued_bytes);
    conversation_request_close(request);
    return rc;
  }
  if (event.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE)
    return H2_PAL_ERR_WOULD_BLOCK;
  const bool terminal = event.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR;
  if (terminal) {
    snprintf(request->terminal_error_code,
             sizeof(request->terminal_error_code), "%s",
             event.error_code != NULL ? event.error_code : "");
    request->terminal_retryable = event.retryable;
    if (request->service->client_config.log != NULL) {
      char message[160];
      snprintf(message, sizeof(message), "input_rejected code=%s retryable=%s",
               request->terminal_error_code,
               event.retryable ? "true" : "false");
      (void)h2_pal_log_write(request->service->client_config.log,
                             H2_PAL_LOG_ERROR, "gizclaw/conversation", message);
    }
    request->notification_terminal_result = H2_PAL_ERR_IO;
  }
  request->dispatch_event = event;
  rc = conversation_queue_notification(request, terminal);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return rc;
  if (rc == H2_PAL_OK && terminal)
    rc = H2_PAL_ERR_IO;
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_ERROR, "conversation",
        "event_dispatch_failed", request->identity, rc, (int)event.kind,
        request->queued_frames, request->queued_bytes);
    conversation_request_close(request);
    return rc;
  }
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
  atomic_init(&request->input_empty, false);
  atomic_init(&request->wire_ready, false);
  atomic_init(&request->control_ready, false);
  atomic_init(&request->notification_done, false);
  atomic_init(&request->notification_suppressed, false);
  atomic_init(&request->terminal, false);
  atomic_init(&request->media_uplink_eos, false);
  atomic_init(&request->audio_result, H2_PAL_OK);
  atomic_init(&request->cancel_source, H2_GIZCLAW_CANCEL_UNSPECIFIED);
  atomic_init(&request->queued_frames, 0u);
  atomic_init(&request->queued_bytes, 0u);
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
    if (request->input_mutex != NULL)
      (void)h2_pal_mutex_destroy(service->config.sync, request->input_mutex);
    audio_ring_deinit(&request->opus_uplink);
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
  (void)h2_pal_mutex_destroy(request->service->config.sync,
                             request->input_mutex);
  h2_pal_mem_free(request->service->client_config.allocator, request->encoder);
  audio_ring_deinit(&request->opus_uplink);
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
  /* The downlink belongs to the connection, not to this route: products
   * create and release a route per input, and audio the server sends after
   * that input still plays. It lives until the Service is deinitialized. */
  if (service->conversation_downlink == NULL) {
    const h2_pal_result_t downlink_rc =
        downlink_create(service, &service->conversation_downlink);
    if (downlink_rc != H2_PAL_OK) {
      (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
      return downlink_rc;
    }
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
  /* A repeated end is a no-op and must not clear audio that arrived after
   * the real release. */
  const bool releasing = !start && !service->audio_ended;
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
  /* Releasing the input clears what is buffered at that moment; whatever the
   * server sends afterwards plays. */
  if (rc == H2_PAL_OK && releasing && speech == NULL && conversation != NULL)
    h2_gizclaw_conversation_downlink_flush_internal(service);
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
    rc = h2_gizclaw_operation_cancel(request->operation);
    record_audio_control(service, conversation, "cancel_requested", rc,
                      H2_PAL_LOG_DEBUG, log);
  } else {
    record_audio_control(service, conversation, "cancel_no_request", rc,
                         H2_PAL_LOG_DEBUG, log);
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
  /* Hanging up or switching Workspace stops what is playing. A new input
   * replacing this one does not: releasing it is what clears the buffer. */
  if (source != H2_GIZCLAW_CANCEL_RESTART)
    h2_gizclaw_conversation_downlink_flush_internal(service);
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
