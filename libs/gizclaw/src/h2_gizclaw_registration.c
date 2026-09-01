#include "h2_gizclaw_registration.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_registration_internal.h"

#include "h2_gizclaw_service_internal.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <stdatomic.h>
#include <string.h>

struct h2_gizclaw_registration_request {
  h2_gizclaw_async_rpc_t *rpc;
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_registration_completion_fn completion;
  void *completion_user;
  h2_gizclaw_operation_result_t operation_result;
  h2_gizclaw_registration_result_t registration;
  atomic_bool terminal;
};

int h2_gizclaw_registration_encode_request(const char *token, uint8_t *out,
                                           size_t capacity, size_t *out_len) {
  if (token == NULL || token[0] == '\0' || out == NULL || out_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t token_len = strlen(token);
  if (token_len >= sizeof(((gizclaw_rpc_v1_ServerRegisterRequest *)0)->token))
    return H2_PAL_ERR_TRUNCATED;
  gizclaw_rpc_v1_ServerRegisterRequest request =
      gizclaw_rpc_v1_ServerRegisterRequest_init_zero;
  memcpy(request.token, token, token_len + 1u);
  pb_ostream_t output = pb_ostream_from_buffer(out, capacity);
  if (!pb_encode(&output, gizclaw_rpc_v1_ServerRegisterRequest_fields,
                 &request)) {
    return H2_PAL_ERR_FORMAT;
  }
  *out_len = output.bytes_written;
  return H2_PAL_OK;
}

int h2_gizclaw_registration_decode_response(
    const uint8_t *data, size_t len,
    h2_gizclaw_registration_result_t *out_registration) {
  if (data == NULL || out_registration == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_registration, 0, sizeof(*out_registration));
  gizclaw_rpc_v1_ServerRegisterResponse decoded =
      gizclaw_rpc_v1_ServerRegisterResponse_init_zero;
  pb_istream_t input = pb_istream_from_buffer(data, len);
  if (!pb_decode(&input, gizclaw_rpc_v1_ServerRegisterResponse_fields,
                 &decoded) ||
      decoded.runtime_profile_name[0] == '\0') {
    return H2_PAL_ERR_FORMAT;
  }
  memcpy(out_registration->runtime_profile_name, decoded.runtime_profile_name,
         sizeof(out_registration->runtime_profile_name));
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_client_register(h2_gizclaw_client_t *client, const char *token,
                           h2_gizclaw_registration_result_t *out_result) {
  if (out_result != NULL)
    memset(out_result, 0, sizeof(*out_result));
  if (client == NULL || token == NULL || token[0] == '\0' ||
      out_result == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint8_t request_bytes[gizclaw_rpc_v1_ServerRegisterRequest_size];
  size_t request_len = 0u;
  h2_pal_result_t result =
      (h2_pal_result_t)h2_gizclaw_registration_encode_request(
          token, request_bytes, sizeof(request_bytes), &request_len);
  h2_gizclaw_rpc_response_t response = {0};
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_REGISTER,
        (h2_gizclaw_rpc_bytes_t){.data = request_bytes, .len = request_len},
        &response);
  }
  if (result == H2_PAL_OK && response.has_error)
    result = H2_PAL_ERR_IO;
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_gizclaw_registration_decode_response(
        response.result_payload, response.result_payload_len, out_result);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  return result;
}

static void registration_rpc_complete(void *user,
                                      h2_gizclaw_async_rpc_t *rpc) {
  const h2_gizclaw_operation_result_t *operation_result =
      h2_gizclaw_async_rpc_operation_result(rpc);
  const h2_gizclaw_rpc_response_t *response =
      h2_gizclaw_async_rpc_response(rpc);
  h2_gizclaw_registration_request_t *request = user;
  h2_gizclaw_operation_result_t result = *operation_result;
  if (result.result == H2_PAL_OK && (response == NULL || response->has_error)) {
    result.result = H2_PAL_ERR_IO;
  }
  if (result.result == H2_PAL_OK) {
    result.result = (h2_pal_result_t)h2_gizclaw_registration_decode_response(
        response->result_payload, response->result_payload_len,
        &request->registration);
  }
  request->operation_result = result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request);
}

h2_pal_result_t h2_gizclaw_service_register_async(
    h2_gizclaw_service_t *service, uint64_t identity, const char *token,
    uint32_t timeout_ms, h2_gizclaw_registration_completion_fn completion,
    void *user, h2_gizclaw_registration_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || token == NULL || token[0] == '\0' ||
      timeout_ms == 0u || completion == NULL || out_request == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint8_t request_bytes[gizclaw_rpc_v1_ServerRegisterRequest_size];
  size_t request_len = 0u;
  const h2_pal_result_t encode_result =
      (h2_pal_result_t)h2_gizclaw_registration_encode_request(
          token, request_bytes, sizeof(request_bytes), &request_len);
  if (encode_result != H2_PAL_OK)
    return encode_result;

  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_registration_request_t *registration_request =
      h2_pal_mem_alloc(allocator, sizeof(*registration_request));
  if (registration_request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(registration_request, 0, sizeof(*registration_request));
  registration_request->allocator = allocator;
  registration_request->completion = completion;
  registration_request->completion_user = user;
  const h2_pal_result_t result = h2_gizclaw_service_rpc_call_async(
      service, identity, H2_GIZCLAW_RPC_SERVER_REGISTER,
      (h2_gizclaw_rpc_bytes_t){.data = request_bytes, .len = request_len},
      timeout_ms, registration_rpc_complete, registration_request,
      &registration_request->rpc);
  if (result != H2_PAL_OK) {
    h2_pal_mem_free(allocator, registration_request);
    return result;
  }
  *out_request = registration_request;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_registration_request_cancel(
    h2_gizclaw_registration_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_async_rpc_cancel(request->rpc);
}

h2_pal_result_t h2_gizclaw_registration_request_wait(
    h2_gizclaw_registration_request_t *request, uint32_t timeout_ms) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_async_rpc_wait(request->rpc, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_registration_request_operation_result(
    const h2_gizclaw_registration_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    return NULL;
  }
  return &request->operation_result;
}

const h2_gizclaw_registration_result_t *
h2_gizclaw_registration_request_response(
    const h2_gizclaw_registration_request_t *request) {
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_registration_request_operation_result(request);
  return result != NULL && result->result == H2_PAL_OK
             ? &request->registration
             : NULL;
}

void h2_gizclaw_registration_request_release(
    h2_gizclaw_registration_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    return;
  }
  h2_gizclaw_async_rpc_release(request->rpc);
  h2_pal_mem_free(request->allocator, request);
}
