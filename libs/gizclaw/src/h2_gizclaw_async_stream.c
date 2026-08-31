#include "h2_gizclaw_service_internal.h"

#include <stdatomic.h>
#include <string.h>

struct h2_gizclaw_async_stream {
  h2_gizclaw_operation_t *operation;
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_rpc_method_t method;
  uint8_t *payload;
  size_t payload_len;
  uint32_t timeout_ms;
  h2_gizclaw_rpc_request_t *request;
  h2_gizclaw_rpc_response_t response;
  h2_gizclaw_async_stream_event_fn on_event;
  h2_gizclaw_async_stream_completion_fn completion;
  void *callback_user;
  const h2_gizclaw_cancel_token_t *cancel_token;
  const h2_gizclaw_rpc_stream_event_t *dispatch_event;
  bool write_finished;
  atomic_bool terminal;
};

static h2_pal_result_t dispatch_stream_event(void *user) {
  h2_gizclaw_async_stream_t *stream = user;
  return stream->on_event(stream->callback_user, stream,
                          stream->dispatch_event);
}

static int receive_stream_event(void *user,
                                const h2_gizclaw_rpc_stream_event_t *event) {
  h2_gizclaw_async_stream_t *stream = user;
  stream->dispatch_event = event;
  const h2_pal_result_t rc = h2_gizclaw_operation_dispatch_call(
      stream->cancel_token, dispatch_stream_event, stream);
  stream->dispatch_event = NULL;
  return rc;
}

static h2_pal_result_t stream_poll(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  (void)client;
  h2_gizclaw_async_stream_t *stream = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    h2_gizclaw_rpc_request_cancel(stream->request);
    h2_gizclaw_rpc_request_destroy(stream->request);
    stream->request = NULL;
    return H2_PAL_ERR_CLOSED;
  }
  if (!stream->write_finished) {
    const h2_pal_result_t rc =
        (h2_pal_result_t)h2_gizclaw_rpc_request_finish_write(stream->request);
    if (rc != H2_PAL_OK)
      return rc;
    stream->write_finished = true;
  }
  const h2_pal_result_t rc =
      (h2_pal_result_t)h2_gizclaw_rpc_request_result(stream->request,
                                                     &stream->response);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return rc;
  h2_gizclaw_rpc_request_destroy(stream->request);
  stream->request = NULL;
  return rc;
}

static h2_pal_result_t stream_start(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_async_stream_t *stream = user;
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  stream->cancel_token = cancel_token;
  const h2_pal_result_t rc =
      (h2_pal_result_t)h2_gizclaw_client_rpc_request_start_stream(
          client, stream->method,
          (h2_gizclaw_rpc_bytes_t){.data = stream->payload,
                                   .len = stream->payload_len},
          stream->timeout_ms, receive_stream_event, stream, &stream->request);
  return rc == H2_PAL_OK ? stream_poll(user, client, cancel_token) : rc;
}

static void stream_complete(
    void *user, h2_gizclaw_operation_t *operation,
    const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  h2_gizclaw_async_stream_t *stream = user;
  atomic_store_explicit(&stream->terminal, true, memory_order_release);
  stream->completion(stream->callback_user, stream, result,
                     result->result == H2_PAL_OK ? &stream->response : NULL);
}

h2_pal_result_t h2_gizclaw_service_rpc_stream_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t params_payload,
    uint32_t timeout_ms, h2_gizclaw_async_stream_event_fn on_event,
    h2_gizclaw_async_stream_completion_fn completion, void *user,
    h2_gizclaw_async_stream_t **out_stream) {
  if (out_stream != NULL)
    *out_stream = NULL;
  if (service == NULL || method <= 0 || timeout_ms == 0u ||
      on_event == NULL || completion == NULL || out_stream == NULL ||
      (params_payload.data == NULL && params_payload.len != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_async_stream_t *stream =
      h2_pal_mem_alloc(allocator, sizeof(*stream));
  if (stream == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(stream, 0, sizeof(*stream));
  stream->allocator = allocator;
  stream->method = method;
  stream->timeout_ms = timeout_ms;
  stream->on_event = on_event;
  stream->completion = completion;
  stream->callback_user = user;
  if (params_payload.len > 0u) {
    stream->payload = h2_pal_mem_alloc(allocator, params_payload.len);
    if (stream->payload == NULL) {
      h2_pal_mem_free(allocator, stream);
      return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(stream->payload, params_payload.data, params_payload.len);
    stream->payload_len = params_payload.len;
  }
  const h2_pal_result_t rc = h2_gizclaw_service_submit_async_internal(
      service, identity, stream_start, stream_poll, stream_complete, stream,
      &stream->operation);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, stream->payload);
    h2_pal_mem_free(allocator, stream);
    return rc;
  }
  *out_stream = stream;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_async_stream_cancel(h2_gizclaw_async_stream_t *stream) {
  return stream == NULL ? H2_PAL_ERR_INVALID_ARG
                        : h2_gizclaw_operation_cancel(stream->operation);
}

void h2_gizclaw_async_stream_release(h2_gizclaw_async_stream_t *stream) {
  if (stream == NULL ||
      !atomic_load_explicit(&stream->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(stream->operation);
  h2_pal_mem_free(stream->allocator, stream->response.result_payload);
  h2_pal_mem_free(stream->allocator, stream->response.error_message);
  h2_pal_mem_free(stream->allocator, stream->payload);
  h2_pal_mem_free(stream->allocator, stream);
}
