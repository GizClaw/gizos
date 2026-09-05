#include "h2_gizclaw_client.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/system.pb.h"
#include "pb_decode.h"

static const char peer_delete_tag;

h2_pal_result_t
h2_gizclaw_req_create_peer_delete(h2_gizclaw_service_t *service,
                                  uint64_t identity, uint32_t timeout_ms,
                                  h2_gizclaw_req_t **out_request) {
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_PEER_DELETE, &peer_delete_tag,
      (h2_gizclaw_rpc_bytes_t){0}, timeout_ms, out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_peer_delete(const h2_gizclaw_req_t *request) {
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &peer_delete_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_ServerPeerDeleteResponse decoded =
      gizclaw_rpc_v1_ServerPeerDeleteResponse_init_zero;
  pb_istream_t input = pb_istream_from_buffer(response->result_payload,
                                              response->result_payload_len);
  return pb_decode(&input, gizclaw_rpc_v1_ServerPeerDeleteResponse_fields,
                   &decoded)
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_gizclaw_rpc_peer_delete(h2_gizclaw_service_t *service,
                                           uint32_t timeout_ms) {
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_create_peer_delete(service, 0u, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_peer_delete(request);
  h2_gizclaw_req_release(request);
  return rc;
}
