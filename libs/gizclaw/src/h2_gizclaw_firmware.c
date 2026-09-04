#include "h2_gizclaw_firmware.h"
#include "h2_gizclaw_service_internal.h"
#include "payload/firmware.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <string.h>

static const char firmware_get_tag;

h2_pal_result_t h2_gizclaw_req_create_firmware_get(
    h2_gizclaw_service_t *service, uint64_t identity, int32_t channel,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (channel <= 0)
    return H2_PAL_ERR_INVALID_ARG;
  const gizclaw_rpc_v1_FirmwareGetRequest message = {
      .channel = (gizclaw_rpc_v1_FirmwareChannelName)channel};
  /* Enough for a positive int32, not just the currently known enum values. */
  uint8_t payload[6];
  pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
  if (!pb_encode(&stream, gizclaw_rpc_v1_FirmwareGetRequest_fields, &message))
    return H2_PAL_ERR_FORMAT;
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_FIRMWARE_GET, &firmware_get_tag,
      (h2_gizclaw_rpc_bytes_t){payload, stream.bytes_written}, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_firmware_get(const h2_gizclaw_req_t *request,
                                   h2_gizclaw_firmware_t *out_firmware) {
  if (out_firmware == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_firmware, 0, sizeof(*out_firmware));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &firmware_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_FirmwareGetResponse message =
      gizclaw_rpc_v1_FirmwareGetResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_FirmwareGetResponse_fields,
                 &message) ||
      (int32_t)message.channel <= 0 || message.url[0] == '\0' ||
      message.size <= 0 || strlen(message.sha256) != 64)
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 0; i < 64; ++i) {
    const char c = message.sha256[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return H2_PAL_ERR_FORMAT;
  }
  _Static_assert(sizeof(out_firmware->url) == sizeof(message.url) &&
                     sizeof(out_firmware->description) ==
                         sizeof(message.description) &&
                     sizeof(out_firmware->sha256) == sizeof(message.sha256),
                 "Firmware wire and public text capacities must match");
  out_firmware->channel = (int32_t)message.channel;
  out_firmware->size = message.size;
  out_firmware->has_description = message.has_description;
  memcpy(out_firmware->description, message.description,
         sizeof(message.description));
  memcpy(out_firmware->url, message.url, sizeof(message.url));
  memcpy(out_firmware->sha256, message.sha256, sizeof(message.sha256));
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_rpc_firmware_get(h2_gizclaw_service_t *service, int32_t channel,
                            uint32_t timeout_ms,
                            h2_gizclaw_firmware_t *out_firmware) {
  if (out_firmware == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_firmware, 0, sizeof(*out_firmware));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_firmware_get(service, 0, channel,
                                                          timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_firmware_get(request, out_firmware);
  h2_gizclaw_req_release(request);
  return rc;
}
