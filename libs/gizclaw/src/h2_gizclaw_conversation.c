#include "h2_gizclaw_conversation.h"

#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_pcm_ring.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_task_names.h"
#include "h2_gizclaw_workspace.h"

#include "events/peer_event.pb.h"
#include "gzc_buffer.h"
#include "gzc_client.h"
#include "gzc_common.h"
#include "gzc_event.h"
#include "opus.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_GIZCLAW_CONVERSATION_REQUEST_QUEUE_ITEMS 8u
#define H2_GIZCLAW_CONVERSATION_ENCODE_TASK_STACK_SIZE 65536u
#define H2_GIZCLAW_CONVERSATION_DECODE_TASK_STACK_SIZE 49152u
#define H2_GIZCLAW_CONVERSATION_OPUS_FRAME_SAMPLES 320u
#define H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES                               \
  (H2_GIZCLAW_CONVERSATION_OPUS_FRAME_SAMPLES * sizeof(int16_t))
#define H2_GIZCLAW_CONVERSATION_AUDIO_PERIOD_MS 20u
#define H2_GIZCLAW_CONVERSATION_DECODE_MAX_SAMPLES 5760u
#define H2_GIZCLAW_CONVERSATION_PCM_RING_BYTES                                 \
  (H2_GIZCLAW_CONVERSATION_PCM_CHUNK_MAX_BYTES *                               \
   H2_GIZCLAW_CONVERSATION_REQUEST_QUEUE_ITEMS)

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

typedef struct h2_gizclaw_conversation_pcm_message {
  h2_gizclaw_audio_message_kind_t kind;
  size_t len;
  uint8_t data[H2_GIZCLAW_CONVERSATION_PCM_CHUNK_MAX_BYTES];
} h2_gizclaw_conversation_pcm_message_t;

typedef struct h2_gizclaw_audio_ring {
  h2_gizclaw_service_t *service;
  uint8_t *items;
  size_t item_size;
  size_t capacity;
  atomic_size_t write_index;
  atomic_size_t read_index;
  atomic_bool closed;
} h2_gizclaw_audio_ring_t;

struct h2_gizclaw_conversation_request {
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_conversation_t *conversation;
  h2_gizclaw_pcm_ring_t pcm_uplink;
  h2_gizclaw_audio_ring_t opus_uplink;
  h2_gizclaw_audio_ring_t opus_downlink;
  h2_gizclaw_pcm_ring_t pcm_downlink;
  h2_pal_task_t *encode_task;
  h2_pal_task_t *decode_task;
  h2_gizclaw_conversation_request_event_fn on_event;
  h2_gizclaw_conversation_request_completion_fn completion;
  void *user;
  char workspace_name[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  size_t workspace_name_len;
  uint64_t generation;
  int timeout_ms;
  h2_gizclaw_conversation_request_message_t pending_message;
  h2_gizclaw_conversation_request_message_t pending_downlink_message;
  h2_gizclaw_conversation_pcm_message_t dispatch_pcm_message;
  h2_gizclaw_conversation_event_t pending_terminal_event;
  h2_gizclaw_conversation_event_t dispatch_event;
  uint64_t identity;
  atomic_size_t queued_frames;
  atomic_size_t queued_bytes;
  atomic_size_t reply_frames;
  atomic_size_t reply_bytes;
  h2_gizclaw_operation_result_t operation_result;
  bool has_pending_message;
  bool has_pending_downlink_message;
  atomic_bool committed;
  atomic_bool terminal;
  atomic_bool downlink_eos;
  atomic_int audio_result;
  bool terminal_waiting_for_audio;
};

struct h2_gizclaw_conversation {
  h2_gizclaw_client_t *client;
  const h2_pal_mem_api_t *allocator;
  gzc_client_t *gzc;
  gzc_event_stream_t *events;
  uint64_t generation;
  char workspace_name[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  char stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  char response_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  char transcript_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  char assistant_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  uint64_t sequence;
  bool input_ready;
  bool committed;
  bool canceled;
  bool terminal_pending;
  bool terminal_has_error;
  bool terminal_retryable;
  bool pending_peer_event;
  gzc_peer_event_t peer_event;
  uint8_t audio[H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES];
  char text[H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES + 1u];
  char error_code[65];
};

#if defined(H2_GIZCLAW_TESTING)
static h2_gizclaw_test_conversation_packet_send_fn s_test_packet_send;
static void *s_test_conversation_ops_user;

void h2_gizclaw_test_set_conversation_ops(
    h2_gizclaw_test_conversation_packet_send_fn packet_send, void *user) {
  s_test_packet_send = packet_send;
  s_test_conversation_ops_user = user;
}
#endif

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
  atomic_init(&ring->write_index, 0u);
  atomic_init(&ring->read_index, 0u);
  atomic_init(&ring->closed, false);
  return H2_PAL_OK;
}

static h2_pal_result_t audio_ring_send(h2_gizclaw_audio_ring_t *ring,
                                       const void *item) {
  if (ring == NULL || ring->items == NULL || item == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (atomic_load_explicit(&ring->closed, memory_order_acquire))
    return H2_PAL_ERR_CLOSED;
  const size_t write =
      atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  const size_t read =
      atomic_load_explicit(&ring->read_index, memory_order_acquire);
  if (write - read >= ring->capacity)
    return H2_PAL_ERR_WOULD_BLOCK;
  memcpy(ring->items + (write % ring->capacity) * ring->item_size, item,
         ring->item_size);
  atomic_store_explicit(&ring->write_index, write + 1u, memory_order_release);
  return H2_PAL_OK;
}

static h2_pal_result_t audio_ring_recv(h2_gizclaw_audio_ring_t *ring,
                                       void *out_item) {
  if (ring == NULL || ring->items == NULL || out_item == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t read =
      atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  const size_t write =
      atomic_load_explicit(&ring->write_index, memory_order_acquire);
  if (read == write) {
    return atomic_load_explicit(&ring->closed, memory_order_acquire)
               ? H2_PAL_ERR_CLOSED
               : H2_PAL_ERR_WOULD_BLOCK;
  }
  memcpy(out_item, ring->items + (read % ring->capacity) * ring->item_size,
         ring->item_size);
  atomic_store_explicit(&ring->read_index, read + 1u, memory_order_release);
  return H2_PAL_OK;
}

static void audio_ring_close(h2_gizclaw_audio_ring_t *ring) {
  if (ring == NULL || ring->items == NULL ||
      atomic_exchange_explicit(&ring->closed, true, memory_order_acq_rel))
    return;
}

static void audio_ring_deinit(h2_gizclaw_audio_ring_t *ring) {
  if (ring == NULL || ring->items == NULL)
    return;
  h2_pal_mem_free(ring->service->config.client_config->allocator, ring->items);
  memset(ring, 0, sizeof(*ring));
}

static h2_pal_result_t
audio_period_sleep(const h2_gizclaw_conversation_request_t *request) {
  return h2_pal_time_sleep_ms(request->service->config.client_config->time,
                              H2_GIZCLAW_CONVERSATION_AUDIO_PERIOD_MS);
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

static void conversation_encode_task(void *user) {
  h2_gizclaw_conversation_request_t *request = user;
  const h2_pal_mem_api_t *mem =
      request->service->config.client_config->allocator;
  h2_pal_result_t rc = H2_PAL_OK;
  const int encoder_size =
      opus_encoder_get_size(H2_GIZCLAW_CONVERSATION_PCM_CHANNELS);
  OpusEncoder *encoder =
      encoder_size > 0 ? h2_pal_mem_alloc(mem, (size_t)encoder_size) : NULL;
  int16_t samples[H2_GIZCLAW_CONVERSATION_OPUS_FRAME_SAMPLES];
  h2_gizclaw_conversation_request_message_t pending = {0};
  bool has_pending = false;
  if (encoder == NULL ||
      opus_encoder_init(encoder, H2_GIZCLAW_CONVERSATION_PCM_SAMPLE_RATE_HZ,
                        H2_GIZCLAW_CONVERSATION_PCM_CHANNELS,
                        OPUS_APPLICATION_VOIP) != OPUS_OK ||
      opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK) {
    rc = H2_PAL_ERR_NO_MEMORY;
  }
  h2_gizclaw_service_log_request(
      request->service, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
      "audio/uplink", "state_idle", request->identity, rc, 0, 0u, 0u);
  bool active = false;
  while (rc == H2_PAL_OK) {
    if (has_pending) {
      rc = audio_ring_send(&request->opus_uplink, &pending);
      if (rc == H2_PAL_ERR_WOULD_BLOCK) {
        rc = audio_period_sleep(request);
        continue;
      }
      if (rc != H2_PAL_OK)
        break;
      has_pending = false;
      if (pending.kind == H2_GIZCLAW_AUDIO_MESSAGE_EOS) {
        h2_gizclaw_service_log_request(
            request->service,
            rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
            "audio/uplink", "state_eos", request->identity, rc, 0,
            atomic_load_explicit(&request->queued_frames, memory_order_relaxed),
            atomic_load_explicit(&request->queued_bytes, memory_order_relaxed));
        break;
      }
    }
    rc = h2_gizclaw_pcm_ring_read(&request->pcm_uplink, (uint8_t *)samples,
                                  H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) {
      if (atomic_load_explicit(&request->committed, memory_order_acquire)) {
        const size_t remaining =
            h2_gizclaw_pcm_ring_available(&request->pcm_uplink);
        if (remaining != 0u) {
          rc = h2_gizclaw_pcm_ring_read(&request->pcm_uplink,
                                        (uint8_t *)samples, remaining);
          if (rc == H2_PAL_OK) {
            memset((uint8_t *)samples + remaining, 0,
                   H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES - remaining);
            rc = audio_encode_opus(encoder, samples, &pending);
            has_pending = rc == H2_PAL_OK;
          }
        } else {
          pending = (h2_gizclaw_conversation_request_message_t){
              .kind = H2_GIZCLAW_AUDIO_MESSAGE_EOS};
          has_pending = true;
          rc = H2_PAL_OK;
        }
      } else {
        rc = H2_PAL_OK;
      }
    } else if (rc == H2_PAL_OK) {
      if (!active) {
        active = true;
        h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_INFO,
                                       "audio/uplink", "state_bos",
                                       request->identity, H2_PAL_OK, 0, 0u, 0u);
      }
      rc = audio_encode_opus(encoder, samples, &pending);
      has_pending = rc == H2_PAL_OK;
    }
    if (rc == H2_PAL_OK)
      rc = audio_period_sleep(request);
  }
  if (encoder != NULL)
    h2_pal_mem_free(mem, encoder);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_CLOSED) {
    int expected = H2_PAL_OK;
    (void)atomic_compare_exchange_strong_explicit(
        &request->audio_result, &expected, rc, memory_order_release,
        memory_order_relaxed);
    h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_ERROR,
                                   "audio/uplink", "failed", request->identity,
                                   rc, 0, 0u, 0u);
  }
}

static void conversation_decode_task(void *user) {
  h2_gizclaw_conversation_request_t *request = user;
  const h2_pal_mem_api_t *mem =
      request->service->config.client_config->allocator;
  h2_pal_result_t rc = H2_PAL_OK;
  const int decoder_size =
      opus_decoder_get_size(H2_GIZCLAW_CONVERSATION_PCM_CHANNELS);
  OpusDecoder *decoder =
      decoder_size > 0 ? h2_pal_mem_alloc(mem, (size_t)decoder_size) : NULL;
  int16_t *decoded = h2_pal_mem_alloc(
      mem, H2_GIZCLAW_CONVERSATION_DECODE_MAX_SAMPLES * sizeof(int16_t));
  if (decoder == NULL || decoded == NULL ||
      opus_decoder_init(decoder, H2_GIZCLAW_CONVERSATION_PCM_SAMPLE_RATE_HZ,
                        H2_GIZCLAW_CONVERSATION_PCM_CHANNELS) != OPUS_OK)
    rc = H2_PAL_ERR_NO_MEMORY;
  bool active = false;
  size_t decoded_bytes = 0u;
  size_t decoded_offset = 0u;
  while (rc == H2_PAL_OK) {
    if (decoded_offset < decoded_bytes) {
      size_t chunk = decoded_bytes - decoded_offset;
      if (chunk > H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES)
        chunk = H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES;
      rc = h2_gizclaw_pcm_ring_write(&request->pcm_downlink,
                                     (const uint8_t *)decoded + decoded_offset,
                                     chunk);
      if (rc == H2_PAL_ERR_WOULD_BLOCK) {
        rc = audio_period_sleep(request);
        continue;
      }
      if (rc != H2_PAL_OK)
        break;
      decoded_offset += chunk;
      rc = audio_period_sleep(request);
      continue;
    }
    h2_gizclaw_conversation_request_message_t input = {0};
    rc = audio_ring_recv(&request->opus_downlink, &input);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) {
      rc = audio_period_sleep(request);
      continue;
    }
    if (rc != H2_PAL_OK)
      break;
    if (input.kind == H2_GIZCLAW_AUDIO_MESSAGE_EOS) {
      atomic_store_explicit(&request->downlink_eos, true, memory_order_release);
      h2_gizclaw_service_log_request(
          request->service,
          rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
          "audio/downlink", "state_eos", request->identity, rc, 0,
          atomic_load_explicit(&request->reply_frames, memory_order_relaxed),
          atomic_load_explicit(&request->reply_bytes, memory_order_relaxed));
      break;
    }
    if (input.kind != H2_GIZCLAW_AUDIO_MESSAGE_OPUS)
      continue;
    if (!active) {
      active = true;
      h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_INFO,
                                     "audio/downlink", "state_bos",
                                     request->identity, H2_PAL_OK, 0, 0u, 0u);
    }
    const int samples = opus_decode(
        decoder, input.len == 0u ? NULL : input.data, (opus_int32)input.len,
        decoded, H2_GIZCLAW_CONVERSATION_DECODE_MAX_SAMPLES, 0);
    if (samples <= 0) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    decoded_bytes = (size_t)samples * sizeof(int16_t);
    decoded_offset = 0u;
  }
  h2_pal_mem_free(mem, decoder);
  h2_pal_mem_free(mem, decoded);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_CLOSED) {
    int expected = H2_PAL_OK;
    (void)atomic_compare_exchange_strong_explicit(
        &request->audio_result, &expected, rc, memory_order_release,
        memory_order_relaxed);
    h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_ERROR,
                                   "audio/downlink", "failed",
                                   request->identity, rc, 0, 0u, 0u);
  }
}

static bool event_transport_failed(int rc) {
  return rc != GZC_OK && rc != GZC_ERR_TIMEOUT && rc != GZC_ERR_WOULD_BLOCK;
}

static int conversation_send_packet(h2_gizclaw_conversation_t *conversation,
                                    uint8_t protocol, const uint8_t *payload,
                                    size_t payload_len) {
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_packet_send != NULL) {
    return s_test_packet_send(s_test_conversation_ops_user, conversation->gzc,
                              protocol, payload, payload_len);
  }
#endif
  return gzc_client_send_packet(conversation->gzc, protocol, payload,
                                payload_len);
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

static bool peer_event_matches_stream(const gzc_peer_event_t *event,
                                      const char *input_stream_id,
                                      char *response_stream_id,
                                      size_t response_stream_id_capacity) {
  if (event == NULL || input_stream_id == NULL)
    return false;
  const char *event_stream_id = peer_event_stream_id(event);
  if (event_stream_id == NULL ||
      stream_id_matches(event_stream_id, input_stream_id))
    return true;
  if (response_stream_id == NULL || response_stream_id_capacity == 0u)
    return false;
  if (response_stream_id[0] == '\0') {
    const size_t event_stream_id_len = strlen(event_stream_id);
    if (event_stream_id_len >= response_stream_id_capacity)
      return false;
    memcpy(response_stream_id, event_stream_id, event_stream_id_len + 1u);
    return true;
  }
  return stream_id_matches(event_stream_id, response_stream_id);
}

static bool conversation_event_matches_routes(
    const gzc_peer_event_t *event, const char *input_stream_id,
    char *response_stream_id, char *transcript_stream_id,
    char *assistant_stream_id, size_t response_stream_id_capacity) {
  char *selected_stream_id = response_stream_id;
  const char *label = peer_event_label(event);
  if (label != NULL && strcmp(label, "transcript") == 0)
    selected_stream_id = transcript_stream_id;
  else if (label != NULL && strcmp(label, "assistant") == 0)
    selected_stream_id = assistant_stream_id;
  return peer_event_matches_stream(event, input_stream_id, selected_stream_id,
                                   response_stream_id_capacity);
}

bool h2_gizclaw_conversation_accepts_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event) {
  return conversation != NULL &&
         conversation_event_matches_routes(
             event, conversation->stream_id, conversation->response_stream_id,
             conversation->transcript_stream_id,
             conversation->assistant_stream_id,
             sizeof(conversation->response_stream_id));
}

bool h2_gizclaw_conversation_has_pending_peer_event_internal(
    const h2_gizclaw_conversation_t *conversation) {
  return conversation != NULL && conversation->pending_peer_event;
}

void h2_gizclaw_conversation_enqueue_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event) {
  if (conversation == NULL || event == NULL || conversation->pending_peer_event)
    return;
  conversation->peer_event = *event;
  conversation->pending_peer_event = true;
}

#if defined(H2_GIZCLAW_TESTING)
bool h2_gizclaw_test_peer_event_matches_stream(const gzc_peer_event_t *event,
                                               const char *stream_id) {
  return peer_event_matches_stream(event, stream_id, NULL, 0u);
}

bool h2_gizclaw_test_peer_event_matches_conversation(
    const gzc_peer_event_t *event, const char *input_stream_id,
    char *response_stream_id, char *transcript_stream_id,
    char *assistant_stream_id, size_t response_stream_id_capacity) {
  return conversation_event_matches_routes(
      event, input_stream_id, response_stream_id, transcript_stream_id,
      assistant_stream_id, response_stream_id_capacity);
}
#endif

static int send_boundary(h2_gizclaw_conversation_t *conversation, bool end,
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
    event.payload.bos.sequence = conversation->sequence++;
    event.payload.bos.timestamp_unix_ms = (int64_t)timestamp_ms;
    event.payload.bos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO;
    (void)snprintf(event.payload.bos.label, sizeof(event.payload.bos.label),
                   "%s", "demo-home");
    (void)snprintf(event.payload.bos.mime_type,
                   sizeof(event.payload.bos.mime_type), "%s", "audio/opus");
  } else {
    event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
    event.which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
    (void)snprintf(event.payload.eos.stream_id,
                   sizeof(event.payload.eos.stream_id), "%s",
                   conversation->stream_id);
    event.payload.eos.sequence = conversation->sequence++;
    event.payload.eos.timestamp_unix_ms = (int64_t)timestamp_ms;
    event.payload.eos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO;
    (void)snprintf(event.payload.eos.label, sizeof(event.payload.eos.label),
                   "%s", "demo-home");
    (void)snprintf(event.payload.eos.mime_type,
                   sizeof(event.payload.eos.mime_type), "%s", "audio/opus");
    if (error_code != NULL) {
      event.payload.eos.has_error = true;
      (void)snprintf(event.payload.eos.error.code,
                     sizeof(event.payload.eos.error.code), "%s", error_code);
      event.payload.eos.error.retryable = true;
    }
  }
  const int gzc_rc =
      h2_gizclaw_event_stream_send_internal(conversation->events, &event);
  if (event_transport_failed(gzc_rc))
    h2_gizclaw_client_event_failure_internal(conversation->client,
                                             conversation);
  return gzc_to_pal(gzc_rc);
}

int h2_gizclaw_conversation_open(h2_gizclaw_client_t *client,
                                 h2_gizclaw_str_t workspace_name,
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
  rc = send_boundary(conversation, false, 0u, NULL);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_client_conversation_release_internal(client, conversation);
    h2_pal_mem_free(allocator, conversation);
    return rc;
  }
  conversation->input_ready = true;
  *out_conversation = conversation;
  return H2_PAL_OK;
}

bool h2_gizclaw_conversation_input_ready(
    const h2_gizclaw_conversation_t *conversation) {
  return conversation != NULL && conversation->input_ready &&
         !conversation->committed && !conversation->canceled &&
         h2_gizclaw_client_conversation_active_internal(conversation->client,
                                                        conversation);
}

int h2_gizclaw_conversation_write_opus(h2_gizclaw_conversation_t *conversation,
                                       const uint8_t *opus, size_t opus_len,
                                       uint64_t timestamp_ms) {
  if (conversation == NULL || opus == NULL || opus_len == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_conversation_input_ready(conversation))
    return H2_PAL_ERR_INVALID_STATE;
  if (opus_len > H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  (void)timestamp_ms;
  return gzc_to_pal(conversation_send_packet(
      conversation, GZC_PROTOCOL_OPUS_PACKET, opus, opus_len));
}

int h2_gizclaw_conversation_commit(h2_gizclaw_conversation_t *conversation,
                                   uint64_t timestamp_ms) {
  if (conversation == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (conversation->canceled)
    return H2_PAL_ERR_CLOSED;
  if (conversation->committed)
    return H2_PAL_OK;
  if (!conversation->input_ready)
    return H2_PAL_ERR_INVALID_STATE;
  const int rc = send_boundary(conversation, true, timestamp_ms, NULL);
  if (rc == H2_PAL_OK) {
    conversation->committed = true;
    conversation->input_ready = false;
  }
  return rc;
}

static int poll_audio(h2_gizclaw_conversation_t *conversation,
                      h2_gizclaw_conversation_event_t *out_event) {
  gzc_buf_t payload;
  gzc_buf_init(&payload);
  uint8_t protocol = 0u;
  const int gzc_rc = h2_gizclaw_client_read_packet_internal(
      conversation->gzc, 0, &protocol, &payload);
  if (gzc_rc != GZC_OK) {
    gzc_buf_free(&payload, gzc_client_platform(conversation->gzc));
    if (gzc_rc == GZC_ERR_CLOSED)
      h2_gizclaw_client_event_failure_internal(conversation->client,
                                               conversation);
    return gzc_rc;
  }
  int rc = GZC_ERR_RPC;
  if (protocol == GZC_PROTOCOL_OPUS_PACKET && payload.len > 0u &&
      payload.len <= sizeof(conversation->audio)) {
    const size_t audio_len = payload.len;
    memcpy(conversation->audio, payload.data, audio_len);
    *out_event = (h2_gizclaw_conversation_event_t){
        .kind = H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO,
        .generation = conversation->generation,
        .audio = conversation->audio,
        .audio_len = audio_len,
    };
    rc = GZC_OK;
  }
  gzc_buf_free(&payload, gzc_client_platform(conversation->gzc));
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

int h2_gizclaw_conversation_poll(h2_gizclaw_conversation_t *conversation,
                                 int timeout_ms,
                                 h2_gizclaw_conversation_event_t *out_event) {
  if (conversation == NULL || out_event == NULL || timeout_ms < 0)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_event, 0, sizeof(*out_event));
  if (conversation->canceled)
    return H2_PAL_ERR_CLOSED;
  if (!h2_gizclaw_client_conversation_active_internal(conversation->client,
                                                      conversation))
    return H2_PAL_ERR_CLOSED;
  int rc = poll_audio(conversation, out_event);
  if (rc == GZC_OK)
    return H2_PAL_OK;
  if (rc != GZC_ERR_TIMEOUT && rc != GZC_ERR_WOULD_BLOCK)
    return gzc_to_pal(rc);
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
    return H2_PAL_OK;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS:
    conversation->terminal_pending = true;
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

void h2_gizclaw_conversation_cancel(h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL || conversation->canceled)
    return;
  if (!conversation->committed && conversation->events != NULL)
    (void)send_boundary(conversation, true, 0u, "canceled");
  conversation->canceled = true;
  conversation->input_ready = false;
}

void h2_gizclaw_conversation_deinit(h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL)
    return;
  h2_gizclaw_conversation_cancel(conversation);
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

static h2_pal_result_t conversation_request_dispatch_event(void *user) {
  h2_gizclaw_conversation_request_t *request = user;
  return request->on_event(request->user, request, &request->dispatch_event);
}

static void
conversation_request_close(h2_gizclaw_conversation_request_t *request) {
  if (request->conversation == NULL)
    return;
  h2_gizclaw_conversation_deinit(request->conversation);
  request->conversation = NULL;
}

static h2_pal_result_t
conversation_request_poll(void *user, h2_gizclaw_client_t *client,
                          const h2_gizclaw_cancel_token_t *cancel_token) {
  (void)client;
  h2_gizclaw_conversation_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_WARN, "conversation", "poll_cancelled",
        request->identity, H2_PAL_ERR_CLOSED, 0, request->queued_frames,
        request->queued_bytes);
    conversation_request_close(request);
    return H2_PAL_ERR_CLOSED;
  }
  const h2_pal_result_t audio_rc = (h2_pal_result_t)atomic_load_explicit(
      &request->audio_result, memory_order_acquire);
  if (audio_rc != H2_PAL_OK) {
    conversation_request_close(request);
    return audio_rc;
  }
  const size_t pcm_available =
      h2_gizclaw_pcm_ring_available(&request->pcm_downlink);
  const bool downlink_eos =
      atomic_load_explicit(&request->downlink_eos, memory_order_acquire);
  request->dispatch_pcm_message.kind = H2_GIZCLAW_AUDIO_MESSAGE_PCM;
  request->dispatch_pcm_message.len =
      pcm_available >= H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES
          ? H2_GIZCLAW_CONVERSATION_OPUS_FRAME_BYTES
          : (downlink_eos ? pcm_available : 0u);
  const h2_pal_result_t pcm_queue_rc =
      request->dispatch_pcm_message.len == 0u
          ? H2_PAL_ERR_WOULD_BLOCK
          : h2_gizclaw_pcm_ring_read(&request->pcm_downlink,
                                     request->dispatch_pcm_message.data,
                                     request->dispatch_pcm_message.len);
  if (pcm_queue_rc == H2_PAL_OK) {
    request->dispatch_event = (h2_gizclaw_conversation_event_t){
        .kind = H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO,
        .generation = request->generation,
        .audio = request->dispatch_pcm_message.data,
        .audio_len = request->dispatch_pcm_message.len,
    };
    const h2_pal_result_t dispatch_rc = h2_gizclaw_operation_dispatch_call(
        cancel_token, conversation_request_dispatch_event, request);
    if (dispatch_rc != H2_PAL_OK)
      return dispatch_rc;
    return H2_PAL_ERR_WOULD_BLOCK;
  } else if (pcm_queue_rc != H2_PAL_ERR_WOULD_BLOCK) {
    return pcm_queue_rc;
  }
  if (request->terminal_waiting_for_audio && downlink_eos &&
      h2_gizclaw_pcm_ring_available(&request->pcm_downlink) == 0u) {
    request->terminal_waiting_for_audio = false;
    request->dispatch_event = request->pending_terminal_event;
    const h2_pal_result_t dispatch_rc = h2_gizclaw_operation_dispatch_call(
        cancel_token, conversation_request_dispatch_event, request);
    conversation_request_close(request);
    return dispatch_rc;
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
  if (!request->has_pending_message) {
    const h2_pal_result_t queue_rc =
        audio_ring_recv(&request->opus_uplink, &request->pending_message);
    if (queue_rc == H2_PAL_OK) {
      request->has_pending_message = true;
    } else if (queue_rc != H2_PAL_ERR_WOULD_BLOCK) {
      h2_gizclaw_service_log_request(
          request->service, H2_PAL_LOG_ERROR, "conversation",
          "queue_recv_failed", request->identity, queue_rc, 0,
          request->queued_frames, request->queued_bytes);
      conversation_request_close(request);
      return queue_rc;
    }
  }
  if (request->has_pending_message) {
    h2_pal_result_t rc;
    if (request->pending_message.kind == H2_GIZCLAW_AUDIO_MESSAGE_EOS) {
      rc = h2_gizclaw_conversation_commit(request->conversation, 0u);
    } else {
      rc = h2_gizclaw_conversation_write_opus(request->conversation,
                                              request->pending_message.data,
                                              request->pending_message.len, 0u);
    }
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    if (rc != H2_PAL_OK) {
      h2_gizclaw_service_log_request(
          request->service, H2_PAL_LOG_ERROR, "conversation",
          request->pending_message.kind == H2_GIZCLAW_AUDIO_MESSAGE_EOS
              ? "commit_send_failed"
              : "opus_send_failed",
          request->identity, rc, 0, request->queued_frames,
          request->queued_bytes);
      conversation_request_close(request);
      return rc;
    }
    request->has_pending_message = false;
    memset(&request->pending_message, 0, sizeof(request->pending_message));
  }

  h2_gizclaw_conversation_event_t event = {0};
  h2_pal_result_t rc =
      h2_gizclaw_conversation_poll(request->conversation, 0, &event);
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
  if (event.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO) {
    if (event.audio_len > sizeof(request->pending_downlink_message.data)) {
      h2_gizclaw_service_log_request(
          request->service, H2_PAL_LOG_ERROR, "audio/downlink",
          "packet_too_large", request->identity, H2_PAL_ERR_FORMAT, 0,
          event.audio_len, sizeof(request->pending_downlink_message.data));
      conversation_request_close(request);
      return H2_PAL_ERR_FORMAT;
    }
    atomic_fetch_add_explicit(&request->reply_frames, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&request->reply_bytes, event.audio_len,
                              memory_order_relaxed);
    request->pending_downlink_message =
        (h2_gizclaw_conversation_request_message_t){
            .kind = H2_GIZCLAW_AUDIO_MESSAGE_OPUS, .len = event.audio_len};
    if (event.audio_len != 0u)
      memcpy(request->pending_downlink_message.data, event.audio,
             event.audio_len);
    request->has_pending_downlink_message = true;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  const bool terminal =
      event.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE ||
      event.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR;
  if (terminal) {
    request->pending_terminal_event = event;
    request->pending_downlink_message =
        (h2_gizclaw_conversation_request_message_t){
            .kind = H2_GIZCLAW_AUDIO_MESSAGE_EOS};
    request->has_pending_downlink_message = true;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  request->dispatch_event = event;
  rc = h2_gizclaw_operation_dispatch_call(
      cancel_token, conversation_request_dispatch_event, request);
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
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_WARN, "conversation", "start_cancelled",
        request->identity, H2_PAL_ERR_CLOSED, 0, request->queued_frames,
        request->queued_bytes);
    return H2_PAL_ERR_CLOSED;
  }
  h2_pal_result_t rc = h2_gizclaw_conversation_open(
      client,
      (h2_gizclaw_str_t){.data = request->workspace_name,
                         .len = request->workspace_name_len},
      request->generation, request->timeout_ms, &request->conversation);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_ERROR, "conversation", "open_failed",
        request->identity, rc, 0, request->queued_frames,
        request->queued_bytes);
    return rc;
  }
  h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_INFO,
                                 "conversation", "opened", request->identity,
                                 H2_PAL_OK, 0, request->queued_frames,
                                 request->queued_bytes);
  return conversation_request_poll(user, client, cancel_token);
}

static void
conversation_request_complete(void *user, h2_gizclaw_operation_t *operation,
                              const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  h2_gizclaw_conversation_request_t *request = user;
  request->operation_result = *result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  h2_gizclaw_service_log_request(
      request->service,
      result->result == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
      "conversation", "completed", request->identity, result->result, 0,
      request->queued_frames, request->queued_bytes);
  h2_gizclaw_service_log_request(
      request->service, H2_PAL_LOG_INFO, "conversation", "reply_summary",
      request->identity, result->result, 0,
      atomic_load_explicit(&request->reply_frames, memory_order_relaxed),
      atomic_load_explicit(&request->reply_bytes, memory_order_relaxed));
  request->completion(request->user, request);
}

h2_pal_result_t h2_gizclaw_service_conversation_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, uint64_t generation, int timeout_ms,
    h2_gizclaw_conversation_request_event_fn on_event,
    h2_gizclaw_conversation_request_completion_fn completion, void *user,
    h2_gizclaw_conversation_request_t **out_request) {
  if (service == NULL || !valid_workspace(workspace_name) || timeout_ms <= 0 ||
      on_event == NULL || completion == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_request = NULL;
  if (service->config.client_config->time == NULL ||
      service->config.client_config->time->vtable == NULL ||
      service->config.client_config->time->vtable->sleep_ms == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
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
  atomic_init(&request->terminal, false);
  atomic_init(&request->downlink_eos, false);
  atomic_init(&request->audio_result, H2_PAL_OK);
  atomic_init(&request->queued_frames, 0u);
  atomic_init(&request->queued_bytes, 0u);
  atomic_init(&request->reply_frames, 0u);
  atomic_init(&request->reply_bytes, 0u);
  h2_pal_result_t rc = h2_gizclaw_pcm_ring_init(
      &request->pcm_uplink, service->config.client_config->allocator,
      H2_GIZCLAW_CONVERSATION_PCM_RING_BYTES);
  if (rc == H2_PAL_OK)
    rc = audio_ring_init(&request->opus_uplink, service,
                         sizeof(h2_gizclaw_conversation_request_message_t),
                         H2_GIZCLAW_CONVERSATION_REQUEST_QUEUE_ITEMS);
  if (rc == H2_PAL_OK)
    rc = audio_ring_init(&request->opus_downlink, service,
                         sizeof(h2_gizclaw_conversation_request_message_t),
                         H2_GIZCLAW_CONVERSATION_REQUEST_QUEUE_ITEMS);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_pcm_ring_init(&request->pcm_downlink,
                                  service->config.client_config->allocator,
                                  H2_GIZCLAW_CONVERSATION_PCM_RING_BYTES);
  const h2_pal_task_options_t encode_options = {
      .name = H2_GIZCLAW_AUDIO_UPLINK_TASK_NAME_VALUE,
      .min_stack_size = H2_GIZCLAW_CONVERSATION_ENCODE_TASK_STACK_SIZE};
  const h2_pal_task_options_t decode_options = {
      .name = H2_GIZCLAW_AUDIO_DOWNLINK_TASK_NAME_VALUE,
      .min_stack_size = H2_GIZCLAW_CONVERSATION_DECODE_TASK_STACK_SIZE};
  if (rc == H2_PAL_OK)
    rc = h2_pal_task_start(service->config.task, &encode_options,
                           conversation_encode_task, request,
                           &request->encode_task);
  if (rc == H2_PAL_OK)
    rc = h2_pal_task_start(service->config.task, &decode_options,
                           conversation_decode_task, request,
                           &request->decode_task);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_service_submit_async_internal(
        service, identity, conversation_request_start,
        conversation_request_poll, conversation_request_complete, request,
        &request->operation);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(service, H2_PAL_LOG_ERROR, "conversation",
                                   "create_failed", identity, rc, 0, 0u, 0u);
    h2_gizclaw_pcm_ring_close(&request->pcm_uplink);
    audio_ring_close(&request->opus_uplink);
    audio_ring_close(&request->opus_downlink);
    h2_gizclaw_pcm_ring_close(&request->pcm_downlink);
    if (request->encode_task != NULL)
      (void)h2_pal_task_join(service->config.task, request->encode_task);
    if (request->decode_task != NULL)
      (void)h2_pal_task_join(service->config.task, request->decode_task);
    h2_gizclaw_pcm_ring_deinit(&request->pcm_uplink);
    audio_ring_deinit(&request->opus_uplink);
    audio_ring_deinit(&request->opus_downlink);
    h2_gizclaw_pcm_ring_deinit(&request->pcm_downlink);
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  h2_gizclaw_service_log_request(service, H2_PAL_LOG_INFO, "conversation",
                                 "created", identity, H2_PAL_OK, 0, 0u, 0u);
  *out_request = request;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_conversation_request_write_pcm(
    h2_gizclaw_conversation_request_t *request, const uint8_t *pcm,
    size_t pcm_len) {
  if (request == NULL || pcm == NULL || pcm_len == 0u ||
      pcm_len > H2_GIZCLAW_CONVERSATION_PCM_CHUNK_MAX_BYTES ||
      pcm_len % sizeof(int16_t) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = H2_PAL_OK;
  if (atomic_load_explicit(&request->committed, memory_order_acquire) ||
      atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    rc = H2_PAL_ERR_CLOSED;
  }
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_pcm_ring_write(&request->pcm_uplink, pcm, pcm_len);
    if (rc == H2_PAL_OK) {
      atomic_fetch_add_explicit(&request->queued_frames, 1u,
                                memory_order_relaxed);
      atomic_fetch_add_explicit(&request->queued_bytes, pcm_len,
                                memory_order_relaxed);
    }
  }
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(
        request->service,
        rc == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_LOG_WARN : H2_PAL_LOG_ERROR,
        "audio/uplink", "ring_write_failed", request->identity, rc, 0,
        request->queued_frames, request->queued_bytes);
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_conversation_request_commit(
    h2_gizclaw_conversation_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = H2_PAL_OK;
  if (atomic_load_explicit(&request->committed, memory_order_acquire) ||
      atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    rc = H2_PAL_ERR_CLOSED;
  }
  if (rc == H2_PAL_OK) {
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&request->committed, &expected,
                                                 true, memory_order_acq_rel,
                                                 memory_order_acquire)) {
      rc = H2_PAL_ERR_CLOSED;
    }
  }
  h2_gizclaw_service_log_request(
      request->service, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
      "conversation", "commit", request->identity, rc, 0,
      request->queued_frames, request->queued_bytes);
  return rc;
}

h2_pal_result_t h2_gizclaw_conversation_request_cancel(
    h2_gizclaw_conversation_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_operation_cancel(request->operation);
}

h2_pal_result_t
h2_gizclaw_conversation_request_wait(h2_gizclaw_conversation_request_t *request,
                                     uint32_t timeout_ms) {
  return request == NULL
             ? H2_PAL_ERR_INVALID_ARG
             : h2_gizclaw_operation_wait(request->operation, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_conversation_request_operation_result(
    const h2_gizclaw_conversation_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return NULL;
  return &request->operation_result;
}

void h2_gizclaw_conversation_request_release(
    h2_gizclaw_conversation_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  h2_gizclaw_pcm_ring_close(&request->pcm_uplink);
  audio_ring_close(&request->opus_uplink);
  audio_ring_close(&request->opus_downlink);
  h2_gizclaw_pcm_ring_close(&request->pcm_downlink);
  (void)h2_pal_task_join(request->service->config.task, request->encode_task);
  (void)h2_pal_task_join(request->service->config.task, request->decode_task);
  h2_gizclaw_pcm_ring_deinit(&request->pcm_uplink);
  audio_ring_deinit(&request->opus_uplink);
  audio_ring_deinit(&request->opus_downlink);
  h2_gizclaw_pcm_ring_deinit(&request->pcm_downlink);
  h2_pal_mem_free(request->service->config.client_config->allocator, request);
}
