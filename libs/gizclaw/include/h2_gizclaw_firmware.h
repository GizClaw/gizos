#ifndef H2_GIZCLAW_FIRMWARE_H
#define H2_GIZCLAW_FIRMWARE_H

#include "h2_gizclaw_service.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  H2_GIZCLAW_FIRMWARE_CHANNEL_STABLE = 1,
  H2_GIZCLAW_FIRMWARE_CHANNEL_BETA = 2,
  H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP = 3,
};

/** Caller-owned metadata snapshot. Channel preserves the server's numeric
 * value, including future channels. URL transport policy belongs to the caller.
 * This API fetches metadata only; it does not download or install firmware. */
typedef struct h2_gizclaw_firmware {
  int32_t channel;
  bool has_description;
  char description[1025];
  char url[2049];
  char sha256[65];
  int64_t size;
} h2_gizclaw_firmware_t;

/** Retain a positive wire channel number without performing network I/O. */
h2_pal_result_t h2_gizclaw_req_create_firmware_get(
    h2_gizclaw_service_t *service, uint64_t identity, int32_t channel,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
/** Parse a matching successful request. Errors clear out_firmware; a successful
 * snapshot remains valid after the request and Service have been released. */
h2_pal_result_t
h2_gizclaw_resp_parse_firmware_get(const h2_gizclaw_req_t *request,
                                   h2_gizclaw_firmware_t *out_firmware);
/** Blocking convenience using create/do/wait/parse/release, without app poll.
 */
h2_pal_result_t
h2_gizclaw_rpc_firmware_get(h2_gizclaw_service_t *service, int32_t channel,
                            uint32_t timeout_ms,
                            h2_gizclaw_firmware_t *out_firmware);

#ifdef __cplusplus
}
#endif
#endif
