#ifndef H2_GIZCLAW_DEBUG_H
#define H2_GIZCLAW_DEBUG_H

#include "h2_gizclaw_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Caller-owned server.runtime.put response. Unknown server modes are retained. */
typedef struct h2_gizclaw_debug_state {
  char mode[64];
} h2_gizclaw_debug_state_t;

/** Create server.runtime.put without network I/O. The mode is copied.
 * Server-supported modes are off, readonly and fullcontrol. This changes the
 * authenticated device's durable server setting, not its local log level.
 * Submit, wait/cancel and release using the normal managed request lifecycle. */
h2_pal_result_t h2_gizclaw_req_create_debug_set(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t mode,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

/** Copy a matching successful terminal response into caller-owned storage.
 * Pending, wrong-type, remote failure and malformed responses fail; on failure
 * out_state is empty. Storage remains valid after request release. */
h2_pal_result_t h2_gizclaw_resp_parse_debug_set(
    const h2_gizclaw_req_t *request, h2_gizclaw_debug_state_t *out_state);

#ifdef __cplusplus
}
#endif
#endif
