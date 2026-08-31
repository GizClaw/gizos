#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <stdatomic.h>
#include <string.h>

#define H2_GIZCLAW_SPEEDTEST_FRAME_BYTES 4096u

struct h2_gizclaw_ping_request {
  h2_gizclaw_async_rpc_t *rpc;
  const h2_pal_mem_api_t *allocator;
  const h2_pal_time_api_t *time;
  h2_gizclaw_ping_completion_fn completion;
  void *completion_user;
  h2_gizclaw_ping_result_t result;
  uint64_t started_ms;
  atomic_bool terminal;
};

struct h2_gizclaw_speedtest_request {
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_client_t *client;
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_speedtest_completion_fn completion;
  void *completion_user;
  h2_gizclaw_rpc_request_t *rpc;
  h2_gizclaw_rpc_response_t response;
  h2_gizclaw_speedtest_result_t result;
  size_t upload_expected;
  size_t download_expected;
  size_t uploaded;
  size_t downloaded;
  uint32_t timeout_ms;
  uint64_t upload_started_ms;
  uint64_t upload_completed_ms;
  uint64_t download_started_ms;
  uint64_t download_completed_ms;
  bool saw_response;
  bool saw_eos;
  bool write_finished;
  atomic_bool terminal;
};

static h2_pal_result_t encode_message(const pb_msgdesc_t *fields,
                                      const void *message, uint8_t *data,
                                      size_t capacity, size_t *out_len) {
  pb_ostream_t stream = pb_ostream_from_buffer(data, capacity);
  if (!pb_encode(&stream, fields, message))
    return H2_PAL_ERR_FORMAT;
  *out_len = stream.bytes_written;
  return H2_PAL_OK;
}

static void ping_rpc_complete(
    void *user, h2_gizclaw_async_rpc_t *rpc,
    const h2_gizclaw_operation_result_t *operation_result,
    const h2_gizclaw_rpc_response_t *response) {
  (void)rpc;
  h2_gizclaw_ping_request_t *request = user;
  h2_gizclaw_operation_result_t result = *operation_result;
  if (result.result == H2_PAL_OK &&
      (response == NULL || response->has_error)) {
    result.result = H2_PAL_ERR_IO;
  }
  if (result.result == H2_PAL_OK) {
    gizclaw_rpc_v1_PingResponse decoded =
        gizclaw_rpc_v1_PingResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                                 response->result_payload_len);
    if (!pb_decode(&stream, gizclaw_rpc_v1_PingResponse_fields, &decoded)) {
      result.result = H2_PAL_ERR_FORMAT;
    } else {
      request->result.server_time_ms = decoded.server_time;
      uint64_t completed_ms = 0u;
      result.result = (h2_pal_result_t)h2_pal_time_get_monotonic_ms(
          request->time, &completed_ms);
      if (result.result == H2_PAL_OK) {
        request->result.round_trip_ms = h2_pal_time_elapsed_ms(
            request->started_ms, completed_ms);
      }
    }
  }
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request, &result,
                      result.result == H2_PAL_OK ? &request->result : NULL);
}

h2_pal_result_t h2_gizclaw_service_ping_async(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_ping_completion_fn completion, void *user,
    h2_gizclaw_ping_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || timeout_ms == 0u || completion == NULL ||
      out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_ping_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = allocator;
  request->time = service->config.client_config->time;
  request->completion = completion;
  request->completion_user = user;
  h2_pal_result_t rc = (h2_pal_result_t)h2_pal_time_get_monotonic_ms(
      request->time, &request->started_ms);
  gizclaw_rpc_v1_PingRequest message = gizclaw_rpc_v1_PingRequest_init_zero;
  uint8_t payload[gizclaw_rpc_v1_PingRequest_size];
  size_t payload_len = 0u;
  if (rc == H2_PAL_OK) {
    rc = encode_message(gizclaw_rpc_v1_PingRequest_fields, &message, payload,
                        sizeof(payload), &payload_len);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_rpc_call_async(
        service, identity, H2_GIZCLAW_RPC_ALL_PING,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        timeout_ms, ping_rpc_complete, request, &request->rpc);
  }
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_ping_request_cancel(h2_gizclaw_ping_request_t *request) {
  return request == NULL ? H2_PAL_ERR_INVALID_ARG
                         : h2_gizclaw_async_rpc_cancel(request->rpc);
}

void h2_gizclaw_ping_request_release(h2_gizclaw_ping_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_async_rpc_release(request->rpc);
  h2_pal_mem_free(request->allocator, request);
}

static int speedtest_frame(void *user,
                           const h2_gizclaw_rpc_stream_event_t *event) {
  h2_gizclaw_speedtest_request_t *request = user;
  if (request == NULL || event == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (event->kind == H2_GIZCLAW_RPC_STREAM_RESPONSE) {
    if (request->saw_response || event->has_error)
      return H2_PAL_ERR_IO;
    gizclaw_rpc_v1_SpeedTestResponse response =
        gizclaw_rpc_v1_SpeedTestResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(event->result_payload.data,
                                                 event->result_payload.len);
    if (!pb_decode(&stream, gizclaw_rpc_v1_SpeedTestResponse_fields,
                   &response) ||
        response.up_content_length < 0 || response.down_content_length < 0 ||
        (uint64_t)response.up_content_length != request->upload_expected ||
        (uint64_t)response.down_content_length != request->download_expected) {
      return H2_PAL_ERR_FORMAT;
    }
    request->saw_response = true;
    return request->download_expected == 0u
               ? H2_PAL_OK
               : h2_gizclaw_client_monotonic_ms_internal(
                     request->client, &request->download_started_ms);
  }
  if (event->kind == H2_GIZCLAW_RPC_STREAM_DATA) {
    if (!request->saw_response || request->saw_eos ||
        request->downloaded > request->download_expected ||
        event->data.len > request->download_expected - request->downloaded)
      return H2_PAL_ERR_FORMAT;
    request->downloaded += event->data.len;
    return H2_PAL_OK;
  }
  if (event->kind != H2_GIZCLAW_RPC_STREAM_EOS || !request->saw_response ||
      request->saw_eos || request->downloaded != request->download_expected)
    return H2_PAL_ERR_FORMAT;
  request->saw_eos = true;
  return request->download_expected == 0u
             ? H2_PAL_OK
             : h2_gizclaw_client_monotonic_ms_internal(
                   request->client, &request->download_completed_ms);
}

static h2_pal_result_t speedtest_poll(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_speedtest_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    h2_gizclaw_rpc_request_cancel(request->rpc);
    h2_gizclaw_rpc_request_destroy(request->rpc);
    request->rpc = NULL;
    return H2_PAL_ERR_CLOSED;
  }
  static const uint8_t upload_frame[H2_GIZCLAW_SPEEDTEST_FRAME_BYTES];
  if (request->uploaded < request->upload_expected) {
    size_t count = request->upload_expected - request->uploaded;
    if (count > sizeof(upload_frame))
      count = sizeof(upload_frame);
    const h2_pal_result_t rc = (h2_pal_result_t)h2_gizclaw_rpc_request_write(
        request->rpc, upload_frame, count);
    if (rc == H2_PAL_OK)
      request->uploaded += count;
    return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
  }
  if (!request->write_finished) {
    h2_pal_result_t rc =
        (h2_pal_result_t)h2_gizclaw_rpc_request_finish_write(request->rpc);
    if (rc != H2_PAL_OK)
      return rc;
    request->write_finished = true;
    if (request->upload_expected > 0u) {
      rc = (h2_pal_result_t)h2_gizclaw_client_monotonic_ms_internal(
          client, &request->upload_completed_ms);
      if (rc != H2_PAL_OK)
        return rc;
    }
  }
  h2_pal_result_t rc = (h2_pal_result_t)h2_gizclaw_rpc_request_result(
      request->rpc, &request->response);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return rc;
  h2_gizclaw_rpc_request_destroy(request->rpc);
  request->rpc = NULL;
  if (rc != H2_PAL_OK)
    return rc;
  if (request->response.has_error || !request->saw_response ||
      !request->saw_eos || request->uploaded != request->upload_expected ||
      request->downloaded != request->download_expected)
    return H2_PAL_ERR_IO;
  return H2_PAL_OK;
}

static h2_pal_result_t speedtest_start(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_speedtest_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  request->client = client;
  gizclaw_rpc_v1_SpeedTestRequest message =
      gizclaw_rpc_v1_SpeedTestRequest_init_zero;
  message.up_content_length = (int64_t)request->upload_expected;
  message.down_content_length = (int64_t)request->download_expected;
  uint8_t payload[gizclaw_rpc_v1_SpeedTestRequest_size];
  size_t payload_len = 0u;
  h2_pal_result_t rc = encode_message(gizclaw_rpc_v1_SpeedTestRequest_fields,
                                      &message, payload, sizeof(payload),
                                      &payload_len);
  if (rc == H2_PAL_OK) {
    rc = (h2_pal_result_t)h2_gizclaw_client_rpc_request_start_stream(
        client, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        request->timeout_ms, speedtest_frame, request, &request->rpc);
  }
  if (rc == H2_PAL_OK && request->upload_expected > 0u)
    rc = (h2_pal_result_t)h2_gizclaw_client_monotonic_ms_internal(
        client, &request->upload_started_ms);
  return rc == H2_PAL_OK ? speedtest_poll(user, client, cancel_token) : rc;
}

static uint64_t bits_per_second(uint64_t bytes, uint64_t elapsed_ms) {
  if (bytes == 0u || elapsed_ms == 0u)
    return 0u;
  if (bytes > UINT64_MAX / 8000u)
    return UINT64_MAX;
  return bytes * 8000u / elapsed_ms;
}

static void speedtest_complete(
    void *user, h2_gizclaw_operation_t *operation,
    const h2_gizclaw_operation_result_t *operation_result) {
  (void)operation;
  h2_gizclaw_speedtest_request_t *request = user;
  h2_gizclaw_operation_result_t result = *operation_result;
  if (result.result == H2_PAL_OK) {
    request->result.upload_bytes = request->uploaded;
    request->result.download_bytes = request->downloaded;
    if (request->uploaded > 0u) {
      request->result.upload_elapsed_ms = h2_pal_time_elapsed_ms(
          request->upload_started_ms, request->upload_completed_ms);
      if (request->result.upload_elapsed_ms == 0u)
        request->result.upload_elapsed_ms = 1u;
      request->result.upload_bits_per_second = bits_per_second(
          request->result.upload_bytes, request->result.upload_elapsed_ms);
    }
    if (request->downloaded > 0u) {
      request->result.elapsed_ms = h2_pal_time_elapsed_ms(
          request->download_started_ms, request->download_completed_ms);
      if (request->result.elapsed_ms == 0u)
        request->result.elapsed_ms = 1u;
      request->result.download_bits_per_second = bits_per_second(
          request->result.download_bytes, request->result.elapsed_ms);
    }
  }
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request, &result,
                      result.result == H2_PAL_OK ? &request->result : NULL);
}

h2_pal_result_t h2_gizclaw_service_speedtest_async(
    h2_gizclaw_service_t *service, uint64_t identity, size_t upload_bytes,
    size_t download_bytes, uint32_t timeout_ms,
    h2_gizclaw_speedtest_completion_fn completion, void *user,
    h2_gizclaw_speedtest_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || (upload_bytes == 0u && download_bytes == 0u) ||
      upload_bytes > H2_GIZCLAW_SPEEDTEST_MAX_BYTES ||
      download_bytes > H2_GIZCLAW_SPEEDTEST_MAX_BYTES || timeout_ms == 0u ||
      completion == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_speedtest_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = allocator;
  request->completion = completion;
  request->completion_user = user;
  request->upload_expected = upload_bytes;
  request->download_expected = download_bytes;
  request->timeout_ms = timeout_ms;
  const h2_pal_result_t rc = h2_gizclaw_service_submit_async_internal(
      service, identity, speedtest_start, speedtest_poll, speedtest_complete,
      request, &request->operation);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_speedtest_request_cancel(
    h2_gizclaw_speedtest_request_t *request) {
  return request == NULL ? H2_PAL_ERR_INVALID_ARG
                         : h2_gizclaw_operation_cancel(request->operation);
}

void h2_gizclaw_speedtest_request_release(
    h2_gizclaw_speedtest_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  h2_pal_mem_free(request->allocator, request->response.result_payload);
  h2_pal_mem_free(request->allocator, request->response.error_message);
  h2_pal_mem_free(request->allocator, request);
}
