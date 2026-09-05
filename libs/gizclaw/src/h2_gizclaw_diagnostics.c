#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <string.h>

typedef struct h2_gizclaw_speedtest_request {
  const h2_pal_mem_api_t *allocator;
  size_t upload_expected;
  size_t download_expected;
  size_t downloaded;
  bool saw_response;
  bool saw_eos;
} h2_gizclaw_speedtest_request_t;

static h2_pal_result_t encode_message(const pb_msgdesc_t *fields,
                                      const void *message, uint8_t *data,
                                      size_t capacity, size_t *out_len) {
  pb_ostream_t stream = pb_ostream_from_buffer(data, capacity);
  if (!pb_encode(&stream, fields, message))
    return H2_PAL_ERR_FORMAT;
  *out_len = stream.bytes_written;
  return H2_PAL_OK;
}

static const char ping_tag;

h2_pal_result_t h2_gizclaw_req_create_ping(h2_gizclaw_service_t *service,
                                           uint64_t identity,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || service->client_config.time == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_PingRequest message = gizclaw_rpc_v1_PingRequest_init_zero;
  uint8_t payload[gizclaw_rpc_v1_PingRequest_size];
  size_t payload_len = 0u;
  h2_pal_result_t rc =
      encode_message(gizclaw_rpc_v1_PingRequest_fields, &message, payload,
                     sizeof(payload), &payload_len);
  return rc == H2_PAL_OK
             ? h2_gizclaw_req_create_rpc_internal(
                   service, identity, H2_GIZCLAW_RPC_ALL_PING, &ping_tag,
                   (h2_gizclaw_rpc_bytes_t){payload, payload_len}, timeout_ms,
                   out_request)
             : rc;
}

h2_pal_result_t
h2_gizclaw_resp_parse_ping(const h2_gizclaw_req_t *request,
                           h2_gizclaw_ping_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &ping_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_PingResponse decoded = gizclaw_rpc_v1_PingResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_PingResponse_fields, &decoded))
    return H2_PAL_ERR_FORMAT;
  uint64_t elapsed_ms = 0u;
  rc = h2_gizclaw_req_elapsed_internal(request, &ping_tag, &elapsed_ms);
  if (rc == H2_PAL_OK) {
    out_result->round_trip_ms = elapsed_ms;
    out_result->server_time_ms = decoded.server_time;
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_ping(h2_gizclaw_service_t *service,
                                    uint32_t timeout_ms,
                                    h2_gizclaw_ping_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_create_ping(service, 0u, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_ping(request, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static int speedtest_frame(void *user,
                           const h2_gizclaw_rpc_stream_event_t *event) {
  h2_gizclaw_speedtest_request_t *request = user;
  if (request == NULL || event == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (event->kind == H2_GIZCLAW_RPC_STREAM_RESPONSE) {
    if (event->has_error)
      return h2_gizclaw_rpc_error_result_internal(event->error_code);
    if (request->saw_response)
      return H2_PAL_ERR_FORMAT;
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
    return H2_PAL_OK;
  }
  if (event->kind == H2_GIZCLAW_RPC_STREAM_DATA) {
    if (!request->saw_response || request->saw_eos ||
        (event->data.len > 0u && event->data.data == NULL) ||
        request->downloaded > request->download_expected ||
        event->data.len > request->download_expected - request->downloaded)
      return H2_PAL_ERR_FORMAT;
    // GizClaw v0.13.2 rpc_speed.go writes byte(i) in 32 KiB blocks. That
    // repeats every 256 bytes, independent of transport fragmentation. This
    // checks the benchmark payload, not an authenticated hash or upload data.
    for (size_t i = 0u; i < event->data.len; ++i)
      if (event->data.data[i] != (uint8_t)(request->downloaded + i))
        return H2_PAL_ERR_FORMAT;
    request->downloaded += event->data.len;
    return H2_PAL_OK;
  }
  if (event->kind != H2_GIZCLAW_RPC_STREAM_EOS || !request->saw_response ||
      request->saw_eos || request->downloaded != request->download_expected ||
      event->input_bytes != request->upload_expected || !event->input_finished)
    return H2_PAL_ERR_FORMAT;
  request->saw_eos = true;
  return H2_PAL_OK;
}

static uint64_t bits_per_second(uint64_t bytes, uint64_t elapsed_ms) {
  if (bytes == 0u || elapsed_ms == 0u)
    return 0u;
  if (bytes > UINT64_MAX / 8000u)
    return UINT64_MAX;
  return bytes * 8000u / elapsed_ms;
}

static const char speedtest_tag;

static void speedtest_destroy(void *context) {
  h2_gizclaw_speedtest_request_t *request = context;
  h2_pal_mem_free(request->allocator, request);
}

typedef struct speedtest_sync_io {
  size_t remaining;
} speedtest_sync_io_t;

static h2_pal_result_t speedtest_input_read(void *user, uint8_t *buffer,
                                            size_t capacity,
                                            size_t *out_read) {
  speedtest_sync_io_t *io = user;
  if (io == NULL || buffer == NULL || out_read == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  size_t count = io->remaining < capacity ? io->remaining : capacity;
  /* The fixed request slot is zero-initialized once. Benchmark contents are
   * irrelevant to the server, so advancing the logical length needs no copy. */
  *out_read = count;
  io->remaining -= count;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_req_create_speedtest(h2_gizclaw_service_t *service,
                                uint64_t identity, size_t upload_bytes,
                                size_t download_bytes, uint32_t timeout_ms,
                                h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || ((upload_bytes == 0u) == (download_bytes == 0u)) ||
      upload_bytes > H2_GIZCLAW_SPEEDTEST_MAX_BYTES ||
      download_bytes > H2_GIZCLAW_SPEEDTEST_MAX_BYTES || out_request == NULL ||
      timeout_ms == 0u || timeout_ms > INT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  h2_gizclaw_speedtest_request_t *context =
      h2_pal_mem_alloc(allocator, sizeof(*context));
  if (context == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(context, 0, sizeof(*context));
  context->allocator = allocator;
  context->upload_expected = upload_bytes;
  context->download_expected = download_bytes;
  gizclaw_rpc_v1_SpeedTestRequest message =
      gizclaw_rpc_v1_SpeedTestRequest_init_zero;
  message.up_content_length = (int64_t)upload_bytes;
  message.down_content_length = (int64_t)download_bytes;
  uint8_t payload[gizclaw_rpc_v1_SpeedTestRequest_size];
  size_t payload_len = 0u;
  h2_pal_result_t rc =
      encode_message(gizclaw_rpc_v1_SpeedTestRequest_fields, &message, payload,
                     sizeof(payload), &payload_len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_stream_internal(
        service, identity, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN, &speedtest_tag,
        (h2_gizclaw_rpc_bytes_t){payload, payload_len}, timeout_ms,
        upload_bytes, speedtest_frame, speedtest_destroy, context, out_request);
  if (rc == H2_PAL_OK && download_bytes != 0u)
    h2_gizclaw_req_output_optional_internal(*out_request);
  if (rc != H2_PAL_OK)
    speedtest_destroy(context);
  return rc;
}

h2_pal_result_t
h2_gizclaw_resp_parse_speedtest(const h2_gizclaw_req_t *request,
                                h2_gizclaw_speedtest_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const void *context = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_context_internal(request, &speedtest_tag, &context);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_gizclaw_speedtest_request_t *state = context;
  uint64_t elapsed_ms = 0u;
  rc = h2_gizclaw_req_elapsed_internal(request, &speedtest_tag, &elapsed_ms);
  if (rc != H2_PAL_OK)
    return rc;
  if (!state->saw_response || !state->saw_eos)
    return H2_PAL_ERR_IO;
  out_result->upload_bytes = state->upload_expected;
  out_result->download_bytes = state->downloaded;
  if (state->upload_expected > 0u) {
    out_result->upload_elapsed_ms = elapsed_ms;
    if (out_result->upload_elapsed_ms == 0u)
      out_result->upload_elapsed_ms = 1u;
    out_result->upload_bits_per_second =
        bits_per_second(state->upload_expected, out_result->upload_elapsed_ms);
  }
  if (state->downloaded > 0u) {
    out_result->elapsed_ms = elapsed_ms;
    if (out_result->elapsed_ms == 0u)
      out_result->elapsed_ms = 1u;
    out_result->download_bits_per_second =
        bits_per_second(state->downloaded, out_result->elapsed_ms);
  }
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_rpc_speedtest(h2_gizclaw_service_t *service, size_t upload_bytes,
                         size_t download_bytes, uint32_t timeout_ms,
                         h2_gizclaw_speedtest_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  h2_gizclaw_req_t *request = NULL;
  speedtest_sync_io_t io = {.remaining = upload_bytes};
  h2_pal_result_t rc = h2_gizclaw_req_create_speedtest(
      service, 0u, upload_bytes, download_bytes, timeout_ms, &request);
  uint64_t started = 0u, completed = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_pal_time_get_monotonic_ms(service->client_config.time, &started);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(
        request, &io, upload_bytes != 0u ? speedtest_input_read : NULL,
        NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_pal_time_get_monotonic_ms(service->client_config.time, &completed);
  if (rc == H2_PAL_OK && completed < started)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_speedtest(request, out_result);
  if (rc == H2_PAL_OK) {
    /* Each request has one direction, so wait acknowledges precisely that
     * transfer. Include queueing/control overhead; exclude result parsing. */
    const uint64_t elapsed = completed == started ? 1u : completed - started;
    if (upload_bytes != 0u) {
      out_result->upload_elapsed_ms = elapsed;
      out_result->upload_bits_per_second =
          bits_per_second(out_result->upload_bytes, elapsed);
    } else {
      out_result->elapsed_ms = elapsed;
      out_result->download_bits_per_second =
          bits_per_second(out_result->download_bytes, elapsed);
    }
  }
  h2_gizclaw_req_release(request);
  return rc;
}
