#include "h2_gizclaw_debug.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/system.pb.h"
#include "payload/workspace.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <string.h>

static const char debug_set_tag;
static const char debug_get_tag;

static bool encode_mode(pb_ostream_t *stream, const pb_field_t *field,
                        void *const *arg) {
  const h2_gizclaw_str_t *mode = *arg;
  return pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, (const pb_byte_t *)mode->data, mode->len);
}

static bool decode_mode(pb_istream_t *stream, const pb_field_t *field,
                        void **arg) {
  (void)field;
  h2_gizclaw_debug_state_t *state = *arg;
  const size_t len = stream->bytes_left;
  if (len == 0u || len >= sizeof(state->mode))
    return false;
  if (!pb_read(stream, (pb_byte_t *)state->mode, len) ||
      memchr(state->mode, '\0', len) != NULL)
    return false;
  state->mode[len] = '\0';
  return true;
}

h2_pal_result_t h2_gizclaw_req_create_debug_set(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t mode,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (mode.data == NULL || mode.len == 0u || mode.len >= 64u ||
      memchr(mode.data, '\0', mode.len) != NULL)
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPutRuntimeRequest message =
      gizclaw_rpc_v1_ServerPutRuntimeRequest_init_zero;
  message.debug_mode.funcs.encode = encode_mode;
  message.debug_mode.arg = &mode;
  uint8_t payload[66];
  pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
  if (!pb_encode(&stream, gizclaw_rpc_v1_ServerPutRuntimeRequest_fields,
                 &message))
    return H2_PAL_ERR_FORMAT;
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_RUNTIME_PUT, &debug_set_tag,
      (h2_gizclaw_rpc_bytes_t){payload, stream.bytes_written}, timeout_ms,
      out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_debug_set(
    const h2_gizclaw_req_t *request, h2_gizclaw_debug_state_t *out_state) {
  if (out_state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_state, 0, sizeof(*out_state));
  const h2_gizclaw_rpc_response_t *response = NULL;
  int rc = h2_gizclaw_req_response_internal(request, &debug_set_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_debug_state_t decoded = {0};
  gizclaw_rpc_v1_ServerPutRuntimeResponse message =
      gizclaw_rpc_v1_ServerPutRuntimeResponse_init_zero;
  message.debug_mode.funcs.decode = decode_mode;
  message.debug_mode.arg = &decoded;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPutRuntimeResponse_fields,
                 &message) || decoded.mode[0] == '\0')
    return H2_PAL_ERR_FORMAT;
  *out_state = decoded;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_create_debug_get(h2_gizclaw_service_t *service,
                                                uint64_t identity,
                                                uint32_t timeout_ms,
                                                h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  /* ServerGetRuntimeRequest has no fields: an empty payload is the
   * canonical encoding. */
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_RUNTIME_GET, &debug_get_tag,
      (h2_gizclaw_rpc_bytes_t){NULL, 0u}, timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_debug_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_debug_state_t *out_state) {
  if (out_state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_state, 0, sizeof(*out_state));
  const h2_gizclaw_rpc_response_t *response = NULL;
  int rc = h2_gizclaw_req_response_internal(request, &debug_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_debug_state_t decoded = {0};
  gizclaw_rpc_v1_ServerGetRuntimeResponse message =
      gizclaw_rpc_v1_ServerGetRuntimeResponse_init_zero;
  message.value.debug_mode.funcs.decode = decode_mode;
  message.value.debug_mode.arg = &decoded;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerGetRuntimeResponse_fields,
                 &message) || !message.has_value)
    return H2_PAL_ERR_FORMAT;
  *out_state = decoded;
  return H2_PAL_OK;
}
