#include "h2_gizclaw_registration.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_registration_internal.h"

#include "h2_gizclaw_service_internal.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <string.h>

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

static const char register_tag;

h2_pal_result_t h2_gizclaw_req_create_register(h2_gizclaw_service_t *service,
                                               uint64_t identity,
                                               const char *token,
                                               uint32_t timeout_ms,
                                               h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  uint8_t payload[gizclaw_rpc_v1_ServerRegisterRequest_size];
  size_t payload_len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)h2_gizclaw_registration_encode_request(
      token, payload, sizeof(payload), &payload_len);
  if (rc != H2_PAL_OK)
    return rc;
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_REGISTER, &register_tag,
      (h2_gizclaw_rpc_bytes_t){payload, payload_len}, timeout_ms, out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_register(const h2_gizclaw_req_t *request,
                               h2_gizclaw_registration_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &register_tag, &response);
  if (rc == H2_PAL_OK) {
    rc = response->result_payload_len == 0u
             ? H2_PAL_ERR_FORMAT
             : (h2_pal_result_t)h2_gizclaw_registration_decode_response(
                   response->result_payload, response->result_payload_len,
                   out_result);
  }
  return rc;
}

h2_pal_result_t
h2_gizclaw_rpc_register(h2_gizclaw_service_t *service, const char *token,
                        uint32_t timeout_ms,
                        h2_gizclaw_registration_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_create_register(service, 0u, token, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_register(request, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}
