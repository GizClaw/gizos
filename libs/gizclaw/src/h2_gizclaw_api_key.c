#include "h2_gizclaw_api_key.h"
#include "h2_gizclaw_service_internal.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include <string.h>
static const char create_tag, revoke_tag;
static bool copy_text(char *out, size_t size, h2_gizclaw_str_t text) {
  if (!text.data || !text.len || text.len >= size ||
      memchr(text.data, 0, text.len))
    return false;
  memcpy(out, text.data, text.len);
  out[text.len] = 0;
  return true;
}
static h2_pal_result_t
create_request(h2_gizclaw_service_t *service, uint64_t identity, int method,
               const void *tag, const pb_msgdesc_t *fields, const void *message,
               uint32_t timeout_ms, h2_gizclaw_req_t **out) {
  uint8_t payload[128];
  pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
  if (!pb_encode(&stream, fields, message))
    return H2_PAL_ERR_FORMAT;
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, method, tag,
      (h2_gizclaw_rpc_bytes_t){payload, stream.bytes_written}, timeout_ms, out);
}
h2_pal_result_t h2_gizclaw_req_create_api_key_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t display_name, bool manage_api_keys, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request)
    *out_request = NULL;
  gizclaw_rpc_v1_APIKeyCreateRequest message = {.manage_api_keys =
                                                    manage_api_keys};
  if (!copy_text(message.display_name, sizeof(message.display_name),
                 display_name))
    return H2_PAL_ERR_INVALID_ARG;
  return create_request(service, identity, 96, &create_tag,
                        gizclaw_rpc_v1_APIKeyCreateRequest_fields, &message,
                        timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_api_key_create(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_api_key_t *out_key) {
  if (!out_key)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_key, 0, sizeof(*out_key));
  const h2_gizclaw_rpc_response_t *response = NULL;
  int rc = h2_gizclaw_req_response_internal(request, &create_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_APIKeyCreateResponse message = {0};
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_APIKeyCreateResponse_fields,
                 &message) ||
      !message.has_value || !message.value.name[0] || !message.api_key[0])
    return H2_PAL_ERR_FORMAT;
  memcpy(out_key->name, message.value.name, sizeof(out_key->name));
  memcpy(out_key->secret, message.api_key, sizeof(out_key->secret));
  memset(&message, 0, sizeof(message));
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_create_api_key_revoke(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request)
    *out_request = NULL;
  gizclaw_rpc_v1_APIKeyRevokeRequest message = {0};
  if (!copy_text(message.name, sizeof(message.name), name))
    return H2_PAL_ERR_INVALID_ARG;
  return create_request(service, identity, 98, &revoke_tag,
                        gizclaw_rpc_v1_APIKeyRevokeRequest_fields, &message,
                        timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_api_key_revoke(const h2_gizclaw_req_t *request) {
  const h2_gizclaw_rpc_response_t *response = NULL;
  int rc = h2_gizclaw_req_response_internal(request, &revoke_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_APIKeyRevokeResponse message = {0};
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  return pb_decode(&stream, gizclaw_rpc_v1_APIKeyRevokeResponse_fields,
                   &message)
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
}
static int wait_request(h2_gizclaw_req_t *request) {
  int rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  return rc == H2_PAL_OK
             ? h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER)
             : rc;
}
h2_pal_result_t h2_gizclaw_rpc_api_key_create(h2_gizclaw_service_t *service,
                                              h2_gizclaw_str_t display_name,
                                              bool manage_api_keys,
                                              uint32_t timeout_ms,
                                              h2_gizclaw_api_key_t *out_key) {
  if (!out_key)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_key, 0, sizeof(*out_key));
  h2_gizclaw_req_t *request = NULL;
  int rc = h2_gizclaw_req_create_api_key_create(
      service, 0, display_name, manage_api_keys, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = wait_request(request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_api_key_create(request, out_key);
  h2_gizclaw_req_release(request);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_api_key_revoke(h2_gizclaw_service_t *service,
                                              h2_gizclaw_str_t name,
                                              uint32_t timeout_ms) {
  h2_gizclaw_req_t *request = NULL;
  int rc = h2_gizclaw_req_create_api_key_revoke(service, 0, name, timeout_ms,
                                                &request);
  if (rc == H2_PAL_OK)
    rc = wait_request(request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_api_key_revoke(request);
  h2_gizclaw_req_release(request);
  return rc;
}
