#include "h2_gizclaw_service_internal.h"

#include <limits.h>
#include <string.h>

typedef struct managed_stream {
  h2_gizclaw_rpc_stream_fn on_frame;
  h2_pal_result_t (*admit)(void *);
  void (*detach)(void *);
  void (*received)(void *, h2_gizclaw_req_t *);
  bool receive_ready, sink_notified; /* Service mutex. */
  h2_pal_result_t sink_result; /* Service mutex; one-shot downstream result. */
  bool pcm_source;
  bool opened, input_closed; /* Service mutex. */
  size_t input_expected;
  size_t input_sent; /* Network owner; source task reads under Service mutex. */
  size_t input_ready;
  uint8_t input[H2_GIZCLAW_STREAM_INPUT_BYTES];
  bool input_finished; /* Network owner. */
  bool wire_done;      /* Network owner. */
  bool eos_received;   /* Network owner. */
  size_t data_refs;
  h2_gizclaw_stream_ring_t *ring; /* Service-owned, valid while bound. */
  bool requires_input, requires_output;
  h2_pal_result_t error; /* Service mutex. */
  h2_gizclaw_stream_lane_t lane;
  bool bound, data_ready; /* Service mutex. */
  size_t perf_ingress_frames, perf_ingress_bytes;
  size_t perf_ring_full_waits, perf_data_steps;
  size_t perf_output_calls, perf_output_bytes;
  size_t perf_input_reads, perf_write_attempts;
  size_t perf_write_ok, perf_write_would_block;
} managed_stream_t;

typedef struct h2_gizclaw_managed_request {
  h2_gizclaw_req_t base;
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *operation;
  h2_pal_mutex_t *mutex;
  h2_pal_semaphore_t *completed;
  atomic_uint refs;
  atomic_bool terminal;
  bool started;
  uint64_t identity;
  h2_gizclaw_rpc_method_t method;
  const void *tag;
  h2_gizclaw_operation_run_fn send;
  void (*destroy_context)(void *context);
  void *context;
  managed_stream_t *stream;
  uint32_t timeout_ms;
  uint8_t *payload;
  size_t payload_len;
  h2_gizclaw_rpc_request_t *wire_request;
  h2_gizclaw_rpc_response_t response;
  h2_pal_result_t result;
  uint64_t started_ms;
  uint64_t completed_ms;
  h2_pal_result_t clock_result;
  void *io_user;
  h2_gizclaw_req_input_read_fn input_read;
  h2_gizclaw_req_output_write_fn output_write;
  h2_gizclaw_req_complete_fn on_complete;
} managed_request_t;

static void managed_unref(void *user);

static managed_request_t **stream_lane_slot(h2_gizclaw_service_t *service,
                                            h2_gizclaw_stream_lane_t lane) {
  switch (lane) {
  case H2_GIZCLAW_STREAM_AUDIO_UPLINK:
    return &service->audio_uplink_stream;
  case H2_GIZCLAW_STREAM_AUDIO_DOWNLINK:
    return &service->audio_downlink_stream;
  case H2_GIZCLAW_STREAM_DATA_UPLINK:
    return &service->data_uplink_stream;
  case H2_GIZCLAW_STREAM_DATA_DOWNLINK:
    return &service->data_downlink_stream;
  default:
    return NULL;
  }
}

/* Caller holds Service mutex. A flag wakes the one worker; requests are never
 * queued or rotated. Each independent source/direction owns one active slot. */
static bool stream_has_ready_work(const managed_request_t *request) {
  const managed_stream_t *s = request->stream;
  const h2_gizclaw_service_t *service = request->service;
  const h2_gizclaw_stream_ring_t *ring = s->ring;
  return s->opened && s->bound && s->error == H2_PAL_OK && s->data_refs == 0u &&
         ring != NULL && !service->stopping &&
         ((ring->queued_frames != 0u && !ring->dispatch_ready) ||
          (s->receive_ready && !s->sink_notified) ||
          (s->requires_input && !s->input_closed && s->input_ready == 0u &&
           s->input_sent < s->input_expected));
}

static void stream_mark_ready(managed_request_t *request) {
  managed_stream_t *s = request->stream;
  h2_gizclaw_service_t *service = request->service;
  if (s->data_ready || !stream_has_ready_work(request))
    return;
  s->data_ready = true;
  (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
}

static void stream_clear_ready(managed_request_t *request) {
  managed_stream_t *s = request->stream;
  if (!s->data_ready)
    return;
  s->data_ready = false;
}

static h2_pal_result_t stream_bind(managed_request_t *request) {
  h2_gizclaw_service_t *service = request->service;
  h2_pal_result_t rc = h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  managed_request_t **slot = stream_lane_slot(service, request->stream->lane);
  if (slot == NULL)
    rc = H2_PAL_ERR_INVALID_STATE;
  else if (*slot != NULL)
    rc = H2_PAL_ERR_BUSY;
  else {
    h2_gizclaw_stream_ring_t *ring =
        &service->stream_rings[request->stream->lane];
    memset(ring, 0, sizeof(*ring));
    request->stream->ring = ring;
    *slot = request;
    request->stream->bound = true;
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  return rc;
}

static void stream_unbind_locked(managed_request_t *request) {
  managed_request_t **slot =
      stream_lane_slot(request->service, request->stream->lane);
  stream_clear_ready(request);
  if (slot != NULL && *slot == request)
    *slot = NULL;
  request->stream->bound = false;
  request->stream->ring = NULL;
}

static void stream_consume_locked(managed_request_t *request) {
  managed_stream_t *stream = request->stream;
  h2_gizclaw_stream_ring_t *ring = stream->ring;
  if (ring == NULL || ring->queued_frames == 0u)
    return;
  ring->slots[ring->read_pos] = (h2_gizclaw_stream_slot_t){0};
  ring->read_pos = (ring->read_pos + 1u) % H2_GIZCLAW_STREAM_RING_SLOTS;
  --ring->queued_frames;
  ring->dispatch_ready = false;
  stream_mark_ready(request);
  (void)h2_pal_cond_broadcast(request->service->config.sync,
                              request->service->progress_cond);
}

/* Copy one SDK-borrowed event into a preallocated ring. DATA may be split at
 * slot boundaries. Full rings apply backpressure to the sole network owner;
 * stop/cancel wakes the wait and terminates the request. */
static int stream_ingress(void *user,
                          const h2_gizclaw_rpc_stream_event_t *event) {
  managed_request_t *request = user;
  managed_stream_t *stream = request->stream;
  h2_gizclaw_service_t *service = request->service;
  if (event == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if ((event->result_payload.len != 0u && event->result_payload.data == NULL) ||
      (event->data.len != 0u && event->data.data == NULL) ||
      (event->error_message.len != 0u && event->error_message.data == NULL))
    return H2_PAL_ERR_FORMAT;
  if (event->kind != H2_GIZCLAW_RPC_STREAM_DATA &&
      (event->result_payload.len > H2_GIZCLAW_STREAM_FRAME_BYTES ||
       event->error_message.len >
           H2_GIZCLAW_STREAM_FRAME_BYTES - event->result_payload.len))
    return H2_PAL_ERR_NO_SPACE;

  const size_t total =
      event->kind == H2_GIZCLAW_RPC_STREAM_DATA
          ? event->data.len
          : event->result_payload.len + event->error_message.len;
  size_t offset = 0u;
  bool first = true;
  do {
    h2_pal_result_t rc =
        h2_pal_mutex_lock(service->config.sync, service->mutex);
    if (rc != H2_PAL_OK)
      return rc;
    h2_gizclaw_stream_ring_t *ring = stream->ring;
    if (ring == NULL) {
      (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
      return H2_PAL_ERR_CLOSED;
    }
    if (ring->queued_frames == H2_GIZCLAW_STREAM_RING_SLOTS)
      ++stream->perf_ring_full_waits;
    while (ring->queued_frames == H2_GIZCLAW_STREAM_RING_SLOTS &&
           stream->error == H2_PAL_OK && !service->stopping)
      if (request->operation != NULL && request->operation->cancel_requested)
        break;
      else
        rc = h2_pal_cond_wait(service->config.sync, service->progress_cond,
                              service->mutex, H2_PAL_SYNC_WAIT_FOREVER);
    const bool canceled =
        request->operation != NULL && request->operation->cancel_requested;
    if (rc != H2_PAL_OK || stream->error != H2_PAL_OK || service->stopping ||
        canceled) {
      if (rc == H2_PAL_OK)
        rc = stream->error != H2_PAL_OK ? stream->error : H2_PAL_ERR_CLOSED;
      (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
      return rc;
    }
    h2_gizclaw_stream_slot_t *slot = &ring->slots[ring->write_pos];
    *slot = (h2_gizclaw_stream_slot_t){.event = *event};
    slot->event.input_bytes = stream->input_sent;
    slot->event.input_finished = stream->input_finished;
    if (event->kind == H2_GIZCLAW_RPC_STREAM_DATA) {
      size_t count = total - offset;
      if (count > H2_GIZCLAW_STREAM_FRAME_BYTES)
        count = H2_GIZCLAW_STREAM_FRAME_BYTES;
      if (count != 0u)
        memcpy(slot->payload, event->data.data + offset, count);
      slot->event.result_payload = (h2_gizclaw_rpc_bytes_t){0};
      slot->event.error_message = (h2_gizclaw_rpc_bytes_t){0};
      slot->event.data = (h2_gizclaw_rpc_bytes_t){slot->payload, count};
      slot->bytes = count;
      offset += count;
    } else {
      size_t used = 0u;
      if (event->result_payload.len != 0u) {
        memcpy(slot->payload, event->result_payload.data,
               event->result_payload.len);
        slot->event.result_payload =
            (h2_gizclaw_rpc_bytes_t){slot->payload, event->result_payload.len};
        used = event->result_payload.len;
      }
      slot->event.data = (h2_gizclaw_rpc_bytes_t){0};
      if (event->error_message.len != 0u) {
        memcpy(slot->payload + used, event->error_message.data,
               event->error_message.len);
        slot->event.error_message = (h2_gizclaw_rpc_bytes_t){
            slot->payload + used, event->error_message.len};
        used += event->error_message.len;
      }
      slot->bytes = used;
      offset = total;
    }
    ring->write_pos = (ring->write_pos + 1u) % H2_GIZCLAW_STREAM_RING_SLOTS;
    ++ring->queued_frames;
    ++stream->perf_ingress_frames;
    stream->perf_ingress_bytes += slot->bytes;
    if (event->kind == H2_GIZCLAW_RPC_STREAM_EOS)
      stream->eos_received = true;
    stream_mark_ready(request);
    (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
    (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
    first = false;
  } while (first || offset < total);
  return H2_PAL_OK;
}

/* The task for this lane owns source generation or frame consumption. No SDK
 * call or completion hook runs here. */
bool h2_gizclaw_req_data_ready_internal(h2_gizclaw_service_t *service,
                                        h2_gizclaw_stream_lane_t lane) {
  managed_request_t **slot = stream_lane_slot(service, lane);
  if (slot != NULL && *slot != NULL)
    stream_mark_ready(*slot);
  return slot != NULL && *slot != NULL && (*slot)->stream->data_ready;
}

bool h2_gizclaw_req_data_step_internal(h2_gizclaw_service_t *service,
                                       h2_gizclaw_stream_lane_t lane) {
  const h2_pal_sync_api_t *sync = service->config.sync;
  if (h2_pal_mutex_lock(sync, service->mutex) != H2_PAL_OK)
    return false;
  managed_request_t **slot = stream_lane_slot(service, lane);
  managed_request_t *request =
      slot != NULL && *slot != NULL && (*slot)->stream->data_ready ? *slot
                                                                   : NULL;
  if (request == NULL) {
    (void)h2_pal_mutex_unlock(sync, service->mutex);
    return false;
  }
  stream_clear_ready(request);
  managed_stream_t *stream = request->stream;
  ++stream->perf_data_steps;
  if (stream->error != H2_PAL_OK || service->stopping) {
    (void)h2_pal_mutex_unlock(sync, service->mutex);
    return true;
  }
  ++stream->data_refs;
  h2_gizclaw_stream_ring_t *ring = stream->ring;
  const bool read_input = stream->requires_input && !stream->input_closed &&
                          stream->input_ready == 0u &&
                          stream->input_sent < stream->input_expected;
  h2_gizclaw_stream_slot_t *frame = ring == NULL || ring->queued_frames == 0u
                                        ? NULL
                                        : &ring->slots[ring->read_pos];
  if (frame != NULL && ring->dispatch_ready)
    frame = NULL;
  const bool notify_sink =
      frame == NULL && stream->receive_ready && !stream->sink_notified;
  if (notify_sink)
    stream->sink_notified = true;
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  h2_pal_result_t rc = H2_PAL_OK;
  if (read_input) {
    size_t capacity = stream->input_expected - stream->input_sent;
    if (capacity > sizeof(stream->input))
      capacity = sizeof(stream->input);
    size_t count = 0u;
    if (request->input_read != NULL)
      rc = request->input_read(request->io_user, stream->input, capacity,
                               &count);
    else
      count = capacity;
    ++stream->perf_input_reads;
    if (rc == H2_PAL_OK && count > capacity)
      rc = H2_PAL_ERR_FORMAT;
    (void)h2_pal_mutex_lock(sync, service->mutex);
    if (rc == H2_PAL_OK) {
      stream->input_ready = count;
      stream->input_closed = count == 0u;
    } else if (rc != H2_PAL_ERR_WOULD_BLOCK && stream->error == H2_PAL_OK) {
      stream->error = rc > H2_PAL_OK ? H2_PAL_ERR_IO : rc;
    }
    (void)h2_pal_mutex_unlock(sync, service->mutex);
  }
  if (frame != NULL && frame->event.has_error) {
    /* Frame handlers collapse every remote code except NOT_FOUND into
     * H2_GIZCLAW_ERR_REMOTE. Record the server's code here, on the shared
     * dispatch path, so a streaming failure is as diagnosable as a unary one.
     */
    h2_gizclaw_service_log_request(
        service, H2_PAL_LOG_ERROR, "stream", "remote_error", request->identity,
        H2_GIZCLAW_ERR_REMOTE, frame->event.error_code, 0u,
        frame->event.error_message.len);
  }
  if (frame != NULL)
    rc = (h2_pal_result_t)stream->on_frame(request->context, &frame->event);
  if (notify_sink)
    stream->received(request->context, &request->base);
  if (rc > H2_PAL_OK)
    rc = H2_PAL_ERR_IO;
  (void)h2_pal_mutex_lock(sync, service->mutex);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK &&
      stream->error == H2_PAL_OK)
    stream->error = (h2_pal_result_t)rc;
  if (frame != NULL && rc == H2_PAL_OK &&
      stream->lane == H2_GIZCLAW_STREAM_DATA_DOWNLINK &&
      frame->event.kind == H2_GIZCLAW_RPC_STREAM_DATA &&
      request->output_write != NULL) {
    ring->dispatch_ready = true;
    h2_gizclaw_service_wake_dispatch_internal(service);
  } else if (frame != NULL && rc != H2_PAL_ERR_WOULD_BLOCK)
    stream_consume_locked(request);
  --stream->data_refs;
  if (read_input && rc == H2_PAL_ERR_WOULD_BLOCK && !service->stopping)
    (void)h2_pal_cond_wait(sync, service->progress_cond, service->mutex, 1u);
  stream_mark_ready(request);
  (void)h2_pal_cond_broadcast(sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  return true;
}

bool h2_gizclaw_req_dispatch_output_internal(h2_gizclaw_service_t *service) {
  const h2_pal_sync_api_t *sync = service->config.sync;
  if (h2_pal_mutex_lock(sync, service->mutex) != H2_PAL_OK)
    return false;
  managed_request_t *request = service->data_downlink_stream;
  managed_stream_t *stream = request == NULL ? NULL : request->stream;
  h2_gizclaw_stream_ring_t *ring = stream == NULL ? NULL : stream->ring;
  if (ring == NULL || !ring->dispatch_ready || ring->queued_frames == 0u ||
      request->output_write == NULL) {
    (void)h2_pal_mutex_unlock(sync, service->mutex);
    return false;
  }
  h2_gizclaw_stream_slot_t *slot = &ring->slots[ring->read_pos];
  const size_t remaining = slot->event.data.len - slot->output_offset;
  const uint8_t *data =
      remaining == 0u ? NULL : slot->event.data.data + slot->output_offset;
  ++stream->data_refs;
  (void)h2_pal_mutex_unlock(sync, service->mutex);

  size_t written = 0u;
  h2_pal_result_t rc =
      request->output_write(request->io_user, data, remaining, &written);
  if (rc == H2_PAL_OK && written > remaining)
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && written == 0u && remaining != 0u)
    rc = H2_PAL_ERR_WOULD_BLOCK;
  if (rc > H2_PAL_OK)
    rc = H2_PAL_ERR_IO;

  (void)h2_pal_mutex_lock(sync, service->mutex);
  if (rc == H2_PAL_OK) {
    ++stream->perf_output_calls;
    stream->perf_output_bytes += written;
    slot->output_offset += written;
    if (slot->output_offset == slot->event.data.len)
      stream_consume_locked(request);
  } else if (rc != H2_PAL_ERR_WOULD_BLOCK && stream->error == H2_PAL_OK) {
    stream->error = rc;
    ring->dispatch_ready = false;
  }
  --stream->data_refs;
  stream_mark_ready(request);
  (void)h2_pal_cond_broadcast(sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  return true;
}

static void stream_detach(managed_request_t *request) {
  h2_gizclaw_service_t *service = request->service;
  managed_stream_t *stream = request->stream;
  const h2_pal_sync_api_t *sync = service->config.sync;
  (void)h2_pal_mutex_lock(sync, service->mutex);
  stream->opened =
      false; /* In-flight data work must not become ready on exit. */
  stream_clear_ready(request);
  while (stream->data_refs != 0u)
    (void)h2_pal_cond_wait(sync, service->progress_cond, service->mutex,
                           H2_PAL_SYNC_WAIT_FOREVER);
  stream_unbind_locked(request);
  (void)h2_pal_mutex_unlock(sync, service->mutex);
}

static void managed_unref(void *user) {
  managed_request_t *request = user;
  if (atomic_fetch_sub_explicit(&request->refs, 1u, memory_order_acq_rel) != 1u)
    return;
  h2_gizclaw_service_t *service = request->service;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  h2_gizclaw_operation_release(request->operation);
  if (request->destroy_context != NULL)
    request->destroy_context(request->context);
  if (request->stream != NULL)
    h2_pal_mem_free(allocator, request->stream);
  (void)h2_pal_semaphore_destroy(service->config.sync, request->completed);
  (void)h2_pal_mutex_destroy(service->config.sync, request->mutex);
  h2_pal_mem_free(allocator, request->response.result_payload);
  h2_pal_mem_free(allocator, request->response.error_message);
  h2_pal_mem_free(allocator, request->payload);
  h2_pal_mem_free(allocator, request);
  /* This is the final access to the borrowed service. */
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  --service->request_reference_count;
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

static void managed_settle(void *user, h2_gizclaw_operation_t *operation,
                           const h2_gizclaw_operation_result_t *result) {
  managed_request_t *request = user;
  request->result = result->result;
  if (request->result == H2_PAL_OK && request->response.has_error) {
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_ERROR, "rpc", "remote_error",
        request->identity, H2_GIZCLAW_ERR_REMOTE, request->response.error_code,
        0u, request->response.error_message_len);
    request->result =
        h2_gizclaw_rpc_error_result_internal(request->response.error_code);
  }
  if (request->clock_result == H2_PAL_OK)
    request->clock_result = h2_pal_time_get_monotonic_ms(
        request->service->client_config.time, &request->completed_ms);
  if (request->stream != NULL && request->stream->perf_ingress_frames != 0u) {
    h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_INFO, "stream",
                                   "ingress_perf", request->identity,
                                   request->result,
                                   (int)request->stream->perf_ring_full_waits,
                                   request->stream->perf_ingress_frames,
                                   request->stream->perf_ingress_bytes);
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_INFO, "stream", "output_perf",
        request->identity, request->result,
        (int)request->stream->perf_data_steps,
        request->stream->perf_output_calls, request->stream->perf_output_bytes);
  }
  if (request->stream != NULL && request->stream->requires_input) {
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_INFO, "stream", "input_perf",
        request->identity, request->result,
        (int)request->stream->perf_write_would_block,
        request->stream->perf_write_attempts, request->stream->input_sent);
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_INFO, "stream", "input_state",
        request->identity, request->result,
        request->stream->input_closed ? 1 : 0, request->stream->input_ready,
        request->stream->input_expected);
  }
  operation->result.result = request->result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  (void)h2_pal_semaphore_give(request->service->config.sync,
                              request->completed);
}

static void managed_complete(void *user, h2_gizclaw_operation_t *operation,
                             const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  managed_request_t *request = user;
  request->on_complete(request->io_user, &request->base, result);
}

static void managed_wire_complete(void *user, h2_pal_result_t result) {
  managed_request_t *request = user;
  h2_gizclaw_service_t *service = request->service;
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (request->operation != NULL &&
      request->operation->state == H2_GIZCLAW_OPERATION_PENDING) {
    request->operation->ready = true;
  }
  if (result != H2_PAL_OK && request->stream != NULL &&
      request->stream->error == H2_PAL_OK)
    request->stream->error = result;
  (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

static h2_pal_result_t
managed_stream_network_step(managed_request_t *request,
                            h2_gizclaw_client_t *client,
                            const h2_gizclaw_cancel_token_t *cancel_token) {
  managed_stream_t *stream = request->stream;
  h2_gizclaw_service_t *service = request->service;
  const h2_pal_sync_api_t *sync = service->config.sync;
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  if (request->clock_result != H2_PAL_OK)
    return request->clock_result;
  uint64_t now = 0u;
  h2_pal_result_t rc =
      h2_pal_time_get_monotonic_ms(service->client_config.time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (now < request->started_ms)
    return H2_PAL_ERR_INVALID_STATE;
  if (now - request->started_ms >= request->timeout_ms)
    return H2_PAL_ERR_TIMEOUT;
  (void)h2_pal_mutex_lock(sync, service->mutex);
  rc = stream->error;
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (request->wire_request == NULL) {
    rc = (h2_pal_result_t)h2_gizclaw_rpc_start_stream_internal(
        client, request->method,
        (h2_gizclaw_rpc_bytes_t){request->payload, request->payload_len},
        request->timeout_ms - (uint32_t)(now - request->started_ms),
        stream_ingress, request, &request->wire_request);
    if (rc != H2_PAL_OK)
      return rc;
    if (request->wire_request == NULL)
      return H2_PAL_ERR_INVALID_STATE;
    (void)h2_gizclaw_rpc_set_complete_internal(request->wire_request,
                                               managed_wire_complete, request);
    (void)h2_pal_mutex_lock(sync, service->mutex);
    stream->opened = true;
    stream_mark_ready(request);
    (void)h2_pal_cond_broadcast(sync, service->progress_cond);
    (void)h2_pal_mutex_unlock(sync, service->mutex);
  }
  if (!stream->input_finished && !stream->wire_done) {
    (void)h2_pal_mutex_lock(sync, service->mutex);
    const size_t count = stream->input_ready;
    (void)h2_pal_mutex_unlock(sync, service->mutex);
    if (count != 0u) {
      ++stream->perf_write_attempts;
      rc = (h2_pal_result_t)h2_gizclaw_rpc_write_internal(request->wire_request,
                                                          stream->input, count);
      if (rc == H2_PAL_OK)
        ++stream->perf_write_ok;
      else if (rc == H2_PAL_ERR_WOULD_BLOCK)
        ++stream->perf_write_would_block;
      if (rc != H2_PAL_OK &&
          !(stream->pcm_source && rc == H2_PAL_ERR_WOULD_BLOCK))
        return rc;
      if (rc == H2_PAL_OK) {
        (void)h2_pal_mutex_lock(sync, service->mutex);
        stream->input_sent += count;
        stream->input_ready = 0u;
        stream_mark_ready(request);
        (void)h2_pal_cond_broadcast(sync, service->progress_cond);
        (void)h2_pal_mutex_unlock(sync, service->mutex);
      }
    }
    (void)h2_pal_mutex_lock(sync, service->mutex);
    const bool drained = stream->pcm_source
                             ? stream->input_closed && stream->input_ready == 0u
                             : stream->input_sent == stream->input_expected;
    const bool short_input = stream->requires_input && stream->input_closed &&
                             stream->input_sent != stream->input_expected;
    (void)h2_pal_mutex_unlock(sync, service->mutex);
    if (short_input)
      return H2_PAL_ERR_FORMAT;
    if (!drained && !stream->pcm_source)
      return H2_PAL_ERR_WOULD_BLOCK;
    if (drained) {
      rc = (h2_pal_result_t)h2_gizclaw_rpc_finish_write_internal(
          request->wire_request);
      if (rc == H2_PAL_OK)
        stream->input_finished = true;
      else if (!stream->pcm_source || rc != H2_PAL_ERR_WOULD_BLOCK)
        return rc;
    }
  }
  if (!stream->wire_done) {
    rc = (h2_pal_result_t)h2_gizclaw_rpc_result_internal(request->wire_request,
                                                         &request->response);
    if (rc != H2_PAL_OK)
      return rc;
    if (request->response.has_error)
      return h2_gizclaw_rpc_error_result_internal(request->response.error_code);
    stream->wire_done = true;
  }
  (void)h2_pal_mutex_lock(sync, service->mutex);
  rc = stream->error;
  const h2_gizclaw_stream_ring_t *ring = stream->ring;
  if (rc == H2_PAL_OK &&
      (ring == NULL || ring->queued_frames != 0u || stream->data_refs != 0u))
    rc = H2_PAL_ERR_WOULD_BLOCK;
  if (rc == H2_PAL_OK && (!stream->eos_received || !stream->input_finished))
    rc = stream->pcm_source || stream->received != NULL ? H2_PAL_ERR_FORMAT
                                                        : H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK && stream->received != NULL) {
    stream->receive_ready = true;
    stream_mark_ready(request);
    rc = stream->sink_result;
  }
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  return rc;
}

static h2_pal_result_t
managed_poll(void *user, h2_gizclaw_client_t *client,
             const h2_gizclaw_cancel_token_t *cancel_token) {
  managed_request_t *request = user;
  if (request->stream != NULL)
    return managed_stream_network_step(request, client, cancel_token);
  h2_pal_result_t rc;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    if (request->wire_request != NULL)
      h2_gizclaw_rpc_cancel_internal(request->wire_request);
    rc = H2_PAL_ERR_CLOSED;
  } else {
    if (request->wire_request == NULL) {
      /* A full transport queue did not accept the initial start. Retry under
       * the original execution deadline, not a fresh timeout per attempt. */
      uint64_t now;
      rc = h2_pal_time_get_monotonic_ms(request->service->client_config.time,
                                        &now);
      if (rc != H2_PAL_OK)
        return rc;
      if (now < request->started_ms)
        return H2_PAL_ERR_INVALID_STATE;
      if (now - request->started_ms >= request->timeout_ms)
        return H2_PAL_ERR_TIMEOUT;
      rc = (h2_pal_result_t)h2_gizclaw_rpc_start_internal(
          client, request->method,
          (h2_gizclaw_rpc_bytes_t){request->payload, request->payload_len},
          request->timeout_ms - (uint32_t)(now - request->started_ms),
          &request->wire_request);
      if (rc != H2_PAL_OK)
        return rc;
      if (h2_gizclaw_rpc_set_complete_internal(
              request->wire_request, managed_wire_complete, request) &&
          request->operation != NULL)
        request->operation->notification_driven = true;
    }
    rc = (h2_pal_result_t)h2_gizclaw_rpc_result_internal(request->wire_request,
                                                         &request->response);
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      return rc;
  }
  if (request->wire_request != NULL)
    h2_gizclaw_rpc_destroy_internal(request->wire_request);
  request->wire_request = NULL;
  return rc;
}

static h2_pal_result_t
managed_start(void *user, h2_gizclaw_client_t *client,
              const h2_gizclaw_cancel_token_t *cancel_token) {
  managed_request_t *request = user;
  request->clock_result = h2_pal_time_get_monotonic_ms(
      request->service->client_config.time, &request->started_ms);
  if (request->stream != NULL)
    return managed_stream_network_step(request, client, cancel_token);
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  if (request->send != NULL)
    return request->send(request->context, client, cancel_token);
  const h2_pal_result_t rc = (h2_pal_result_t)h2_gizclaw_rpc_start_internal(
      client, request->method,
      (h2_gizclaw_rpc_bytes_t){request->payload, request->payload_len},
      request->timeout_ms, &request->wire_request);
  if (rc == H2_PAL_OK) {
    const bool notified = h2_gizclaw_rpc_set_complete_internal(
        request->wire_request, managed_wire_complete, request);
    if (!notified && request->operation != NULL)
      request->operation->notification_driven = false;
  } else if (rc == H2_PAL_ERR_WOULD_BLOCK && request->operation != NULL) {
    /* No SDK request exists yet, so completion notification cannot wake the
     * initial transport-admission retry. */
    request->operation->notification_driven = false;
  }
  if (rc == H2_PAL_ERR_WOULD_BLOCK && request->clock_result != H2_PAL_OK)
    return request->clock_result;
  return rc == H2_PAL_OK ? managed_poll(user, client, cancel_token) : rc;
}

static void managed_stop(void *user) {
  managed_request_t *request = user;
  if (request->stream != NULL) {
    stream_detach(request);
    if (request->stream->detach != NULL)
      request->stream->detach(request->context);
    if (request->wire_request != NULL) {
      if (!request->stream->wire_done)
        h2_gizclaw_rpc_cancel_internal(request->wire_request);
      h2_gizclaw_rpc_destroy_internal(request->wire_request);
      request->wire_request = NULL;
    }
  }
}

static h2_pal_result_t managed_do(h2_gizclaw_req_t *base, void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  managed_request_t *request = (managed_request_t *)base;
  const h2_pal_sync_api_t *sync = request->service->config.sync;
  h2_pal_result_t rc = h2_pal_mutex_lock(sync, request->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (request->started ||
      atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    (void)h2_pal_mutex_unlock(sync, request->mutex);
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_result_t (*admit)(void *) =
      request->stream != NULL ? request->stream->admit : NULL;
  if (request->stream != NULL) {
    rc = stream_bind(request);
    if (rc != H2_PAL_OK) {
      (void)h2_pal_mutex_unlock(sync, request->mutex);
      return rc;
    }
  }
  if (admit != NULL) {
    rc = admit(request->context);
    if (rc != H2_PAL_OK) {
      if (request->stream != NULL) {
        (void)h2_pal_mutex_lock(sync, request->service->mutex);
        stream_unbind_locked(request);
        (void)h2_pal_mutex_unlock(sync, request->service->mutex);
      }
      (void)h2_pal_mutex_unlock(sync, request->mutex);
      return rc;
    }
  }
  request->started = true;
  request->io_user = user;
  request->input_read = input_read;
  request->output_write = output_write;
  request->on_complete = on_complete;
  /* One execution reference and one for this call: completion may release the
   * execution reference before submit returns. */
  atomic_fetch_add_explicit(&request->refs, 2u, memory_order_relaxed);
  rc = h2_gizclaw_service_submit_request_internal(
      request->service, request->identity, managed_start,
      request->send != NULL ? NULL : managed_poll, managed_settle,
      on_complete != NULL ? managed_complete : NULL, managed_unref,
      managed_stop, request, request->stream == NULL && request->send == NULL,
      &request->operation);
  if (rc != H2_PAL_OK) {
    request->started = false;
    /* A stream reserves its fixed lane before submission. Random streams have
     * no custom admit hook, so they must still release that slot when the
     * operation queue rejects the request. */
    if (request->stream != NULL || admit != NULL)
      managed_stop(request);
    atomic_fetch_sub_explicit(&request->refs, 1u, memory_order_relaxed);
  }
  (void)h2_pal_mutex_unlock(sync, request->mutex);
  managed_unref(request);
  return rc;
}

static h2_pal_result_t managed_wait(h2_gizclaw_req_t *base,
                                    uint32_t timeout_ms) {
  managed_request_t *request = (managed_request_t *)base;
  if (atomic_load_explicit(&request->terminal, memory_order_acquire))
    return request->result;
  const h2_pal_sync_api_t *sync = request->service->config.sync;
  h2_pal_result_t rc = h2_pal_mutex_lock(sync, request->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  const bool started = request->started;
  (void)h2_pal_mutex_unlock(sync, request->mutex);
  if (!started)
    return H2_PAL_ERR_INVALID_STATE;
  rc = h2_pal_semaphore_take(sync, request->completed, timeout_ms);
  if (rc == H2_PAL_OK) {
    /* A terminal request is a level-triggered condition, not a consumable
     * notification. Pass the wakeup on to concurrent/repeated waiters. */
    (void)h2_pal_semaphore_give(sync, request->completed);
    (void)atomic_load_explicit(&request->terminal, memory_order_acquire);
    return request->result;
  }
  return rc;
}

static h2_pal_result_t managed_cancel(h2_gizclaw_req_t *base) {
  managed_request_t *request = (managed_request_t *)base;
  const h2_pal_sync_api_t *sync = request->service->config.sync;
  h2_pal_result_t rc = h2_pal_mutex_lock(sync, request->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    rc = H2_PAL_OK;
  } else if (request->started) {
    rc = h2_gizclaw_operation_cancel(request->operation);
  } else {
    request->result = H2_PAL_ERR_CLOSED;
    atomic_store_explicit(&request->terminal, true, memory_order_release);
    (void)h2_pal_semaphore_give(sync, request->completed);
  }
  (void)h2_pal_mutex_unlock(sync, request->mutex);
  return rc;
}

static void managed_release(h2_gizclaw_req_t *base) { managed_unref(base); }

static const h2_gizclaw_req_vtable_t managed_vtable = {
    .do_request = managed_do,
    .wait = managed_wait,
    .cancel = managed_cancel,
    .release = managed_release,
};

static h2_pal_result_t
create_request(h2_gizclaw_service_t *service, uint64_t identity,
               h2_gizclaw_rpc_method_t method, const void *tag,
               h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
               void *context, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || tag == NULL || timeout_ms == 0u ||
      timeout_ms > INT32_MAX || out_request == NULL ||
      (payload.len != 0u && payload.data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_sync_api_t *sync = service->config.sync;
  h2_pal_result_t rc = h2_pal_mutex_lock(sync, service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (service->stopping || service->stopped) {
    (void)h2_pal_mutex_unlock(sync, service->mutex);
    return H2_PAL_ERR_CLOSED;
  }
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  managed_request_t *request = h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL) {
    (void)h2_pal_mutex_unlock(sync, service->mutex);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(request, 0, sizeof(*request));
  request->base.vtable = &managed_vtable;
  request->service = service;
  request->identity = identity;
  request->method = method;
  request->tag = tag;
  request->context = context;
  request->timeout_ms = timeout_ms;
  request->clock_result = H2_PAL_ERR_UNAVAILABLE;
  atomic_init(&request->refs, 1u);
  atomic_init(&request->terminal, false);
  const h2_pal_mutex_config_t mutex_config = {
      .name = "$gizclaw/req",
      .allocator = allocator,
  };
  rc = h2_pal_mutex_create(sync, &mutex_config, &request->mutex);
  if (rc == H2_PAL_OK) {
    const h2_pal_semaphore_config_t completed_config = {
        .name = "$gizclaw/req-completed",
        .allocator = allocator,
        .initial_count = 0u,
        .max_count = 1u,
    };
    rc = h2_pal_semaphore_create(sync, &completed_config, &request->completed);
  }
  if (rc == H2_PAL_OK && payload.len != 0u) {
    request->payload = h2_pal_mem_alloc(allocator, payload.len);
    if (request->payload == NULL) {
      rc = H2_PAL_ERR_NO_MEMORY;
    } else {
      memcpy(request->payload, payload.data, payload.len);
      request->payload_len = payload.len;
    }
  }
  if (rc != H2_PAL_OK) {
    if (request->completed != NULL)
      (void)h2_pal_semaphore_destroy(sync, request->completed);
    if (request->mutex != NULL)
      (void)h2_pal_mutex_destroy(sync, request->mutex);
    h2_pal_mem_free(allocator, request);
  } else {
    /* A created request owns the service even before it occupies an active
     * operation slot; Track callbacks have their own, independent references.
     */
    ++service->request_reference_count;
    *out_request = &request->base;
  }
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_rpc_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (method <= 0) {
    if (out_request != NULL)
      *out_request = NULL;
    return H2_PAL_ERR_INVALID_ARG;
  }
  return create_request(service, identity, method, tag, payload, timeout_ms,
                        NULL, out_request);
}

h2_pal_result_t h2_gizclaw_req_create_rpc_context_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    void (*destroy)(void *), void *context, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (method <= 0 || destroy == NULL || context == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *base = NULL;
  h2_pal_result_t rc = create_request(service, identity, method, tag, payload,
                                      timeout_ms, context, &base);
  if (rc == H2_PAL_OK) {
    ((managed_request_t *)base)->destroy_context = destroy;
    *out_request = base;
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_stream_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms, size_t input_bytes,
    h2_gizclaw_rpc_stream_fn on_frame, void (*destroy)(void *), void *context,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || method <= 0 || on_frame == NULL || destroy == NULL ||
      context == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  managed_stream_t *stream =
      h2_pal_mem_alloc(service->client_config.allocator, sizeof(*stream));
  if (stream == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  *stream = (managed_stream_t){
      .on_frame = on_frame,
      .input_expected = input_bytes,
      .lane = input_bytes != 0u ? H2_GIZCLAW_STREAM_DATA_UPLINK
                                : H2_GIZCLAW_STREAM_DATA_DOWNLINK,
      .requires_input = input_bytes != 0u,
      .requires_output = input_bytes == 0u,
  };
  h2_gizclaw_req_t *base = NULL;
  h2_pal_result_t rc = create_request(service, identity, method, tag, payload,
                                      timeout_ms, context, &base);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(service->client_config.allocator, stream);
    return rc;
  }
  managed_request_t *request = (managed_request_t *)base;
  request->stream = stream;
  request->destroy_context = destroy;
  *out_request = base;
  return H2_PAL_OK;
}

void h2_gizclaw_req_output_optional_internal(h2_gizclaw_req_t *base) {
  managed_request_t *request = (managed_request_t *)base;
  if (request != NULL && request->base.vtable == &managed_vtable &&
      request->stream != NULL)
    request->stream->requires_output = false;
}

h2_pal_result_t h2_gizclaw_req_create_sink_stream_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_stream_fn on_frame,
    void (*received)(void *, h2_gizclaw_req_t *),
    h2_pal_result_t (*admit)(void *), void (*detach)(void *),
    void (*destroy)(void *), void *context, bool audio_sink,
    h2_gizclaw_req_t **out_request) {
  if (received == NULL) {
    if (out_request != NULL)
      *out_request = NULL;
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = h2_gizclaw_req_create_stream_internal(
      service, identity, method, tag, payload, timeout_ms, 0u, on_frame,
      destroy, context, out_request);
  if (rc == H2_PAL_OK) {
    managed_stream_t *stream = ((managed_request_t *)*out_request)->stream;
    stream->received = received;
    stream->lane = audio_sink ? H2_GIZCLAW_STREAM_AUDIO_DOWNLINK
                              : H2_GIZCLAW_STREAM_DATA_DOWNLINK;
    stream->requires_output = !audio_sink;
    stream->sink_result = H2_PAL_ERR_WOULD_BLOCK;
    stream->admit = admit;
    stream->detach = detach;
  }
  return rc;
}

void h2_gizclaw_req_sink_done_internal(h2_gizclaw_req_t *base,
                                       h2_pal_result_t result) {
  managed_request_t *request = (managed_request_t *)base;
  h2_gizclaw_service_t *service = request->service;
  if (result == H2_PAL_ERR_WOULD_BLOCK || result > H2_PAL_OK)
    result = H2_PAL_ERR_IO;
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (request->stream->sink_result == H2_PAL_ERR_WOULD_BLOCK)
    request->stream->sink_result = result;
  (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

h2_pal_result_t h2_gizclaw_req_create_pcm_stream_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_stream_fn on_frame, h2_pal_result_t (*admit)(void *),
    void (*detach)(void *), void (*destroy)(void *), void *context,
    h2_gizclaw_req_t **out_request) {
  if (admit == NULL || detach == NULL) {
    if (out_request != NULL)
      *out_request = NULL;
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = h2_gizclaw_req_create_stream_internal(
      service, identity, method, tag, payload, timeout_ms, 0u, on_frame,
      destroy, context, out_request);
  if (rc == H2_PAL_OK) {
    managed_stream_t *stream = ((managed_request_t *)*out_request)->stream;
    stream->pcm_source = true;
    stream->lane = H2_GIZCLAW_STREAM_AUDIO_UPLINK;
    stream->requires_input = false;
    stream->requires_output = false;
    stream->admit = admit;
    stream->detach = detach;
  }
  return rc;
}

bool h2_gizclaw_req_pcm_ready_internal(h2_gizclaw_req_t *base) {
  managed_request_t *request = (managed_request_t *)base;
  h2_gizclaw_service_t *service = request->service;
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  const managed_stream_t *stream = request->stream;
  const bool ready =
      stream->opened && !stream->input_closed && stream->error == H2_PAL_OK;
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  return ready;
}

h2_pal_result_t h2_gizclaw_req_pcm_write_internal(h2_gizclaw_req_t *base,
                                                  const uint8_t *data,
                                                  size_t len) {
  managed_request_t *request = (managed_request_t *)base;
  managed_stream_t *stream = request->stream;
  h2_gizclaw_service_t *service = request->service;
  if (data == NULL || len == 0u || len > sizeof(stream->input))
    return H2_PAL_ERR_INVALID_ARG;
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  h2_pal_result_t rc = stream->error;
  if (rc == H2_PAL_OK) {
    if (stream->input_closed)
      rc = H2_PAL_ERR_CLOSED;
    else if (!stream->opened || stream->input_ready != 0u)
      rc = H2_PAL_ERR_WOULD_BLOCK;
    else {
      memcpy(stream->input, data, len);
      stream->input_ready = len;
      (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
    }
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  return rc;
}

void h2_gizclaw_req_pcm_end_internal(h2_gizclaw_req_t *base,
                                     h2_pal_result_t result) {
  managed_request_t *request = (managed_request_t *)base;
  h2_gizclaw_service_t *service = request->service;
  (void)h2_pal_mutex_lock(service->config.sync, service->mutex);
  request->stream->input_closed = true;
  if (request->stream->error == H2_PAL_OK)
    request->stream->error = result;
  (void)h2_pal_cond_broadcast(service->config.sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
}

h2_pal_result_t h2_gizclaw_req_create_send_internal(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    const void *tag, h2_gizclaw_operation_run_fn send,
    void (*destroy)(void *context), void *context,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (send == NULL || destroy == NULL || context == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *base = NULL;
  h2_pal_result_t rc =
      create_request(service, identity, 0, tag, (h2_gizclaw_rpc_bytes_t){0},
                     timeout_ms, context, &base);
  if (rc != H2_PAL_OK)
    return rc;
  managed_request_t *request = (managed_request_t *)base;
  request->send = send;
  request->destroy_context = destroy;
  *out_request = base;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_context_internal(const h2_gizclaw_req_t *base,
                                                const void *tag,
                                                const void **out_context) {
  if (out_context != NULL)
    *out_context = NULL;
  if (base == NULL || base->vtable != &managed_vtable || out_context == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const managed_request_t *request = (const managed_request_t *)base;
  if (request->tag != tag || request->context == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!atomic_load_explicit(&request->terminal, memory_order_acquire))
    return H2_PAL_ERR_INVALID_STATE;
  if (request->result != H2_PAL_OK)
    return request->result;
  *out_context = request->context;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_response_internal(
    const h2_gizclaw_req_t *base, const void *tag,
    const h2_gizclaw_rpc_response_t **out_response) {
  if (out_response != NULL)
    *out_response = NULL;
  if (base == NULL || base->vtable != &managed_vtable || out_response == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const managed_request_t *request = (const managed_request_t *)base;
  if (request->tag != tag || request->send != NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!atomic_load_explicit(&request->terminal, memory_order_acquire))
    return H2_PAL_ERR_INVALID_STATE;
  if (request->result != H2_PAL_OK)
    return request->result;
  *out_response = &request->response;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_elapsed_internal(const h2_gizclaw_req_t *base,
                                                const void *tag,
                                                uint64_t *out_elapsed_ms) {
  if (out_elapsed_ms == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_elapsed_ms = 0u;
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(base, tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  const managed_request_t *request = (const managed_request_t *)base;
  if (request->clock_result != H2_PAL_OK)
    return request->clock_result;
  if (request->completed_ms < request->started_ms)
    return H2_PAL_ERR_INVALID_STATE;
  *out_elapsed_ms = request->completed_ms - request->started_ms;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_req_input_internal(const h2_gizclaw_req_t *base, const void *tag,
                              h2_gizclaw_rpc_bytes_t *out_input) {
  if (out_input != NULL)
    *out_input = (h2_gizclaw_rpc_bytes_t){0};
  if (base == NULL || base->vtable != &managed_vtable || out_input == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const managed_request_t *request = (const managed_request_t *)base;
  if (request->tag != tag || request->send != NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_input = (h2_gizclaw_rpc_bytes_t){request->payload, request->payload_len};
  return H2_PAL_OK;
}

static h2_pal_result_t request_valid(const h2_gizclaw_req_t *request) {
  return request != NULL && request->vtable != NULL ? H2_PAL_OK
                                                    : H2_PAL_ERR_INVALID_ARG;
}

h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request, void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  if (request_valid(request) != H2_PAL_OK ||
      request->vtable->do_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->vtable->do_request(request, user, input_read, output_write,
                                     on_complete);
}

h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request,
                                    uint32_t timeout_ms) {
  if (request_valid(request) != H2_PAL_OK || request->vtable->wait == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->vtable->wait(request, timeout_ms);
}

h2_pal_result_t
h2_gizclaw_req_wait_dispatch_internal(h2_gizclaw_req_t *request) {
  if (request_valid(request) != H2_PAL_OK || request->vtable != &managed_vtable)
    return H2_PAL_ERR_INVALID_ARG;
  managed_request_t *managed = (managed_request_t *)request;
  for (;;) {
    h2_pal_result_t rc = managed_wait(request, 1u);
    if (rc != H2_PAL_ERR_TIMEOUT)
      return rc;
    size_t dispatched = 0u;
    rc = h2_gizclaw_service_poll(managed->service, 1u, &dispatched);
    if (rc != H2_PAL_OK)
      return rc;
  }
}

h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request) {
  if (request_valid(request) != H2_PAL_OK || request->vtable->cancel == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->vtable->cancel(request);
}

void h2_gizclaw_req_release(h2_gizclaw_req_t *request) {
  if (request_valid(request) == H2_PAL_OK && request->vtable->release != NULL)
    request->vtable->release(request);
}
