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

/** Create server.runtime.get without network I/O. It reads the durable
 * debug access mode the server currently holds for this device, so a UI can
 * show the confirmed mode before the user changes it. Same managed request
 * lifecycle as debug_set. */
h2_pal_result_t h2_gizclaw_req_create_debug_get(h2_gizclaw_service_t *service,
                                                uint64_t identity,
                                                uint32_t timeout_ms,
                                                h2_gizclaw_req_t **out_request);
/** Copy the server's current debug mode from a successful server.runtime.get
 * response. A server that has never stored a mode answers without one; that
 * is a success with an empty `mode`. Pending, wrong-type, remote failure and
 * malformed responses fail with an empty out_state. */
h2_pal_result_t h2_gizclaw_resp_parse_debug_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_debug_state_t *out_state);

/** Library-owned view of the server debug access mode. The Service keeps it
 * current from its own refresh and set requests; a product only reads it. */
typedef struct h2_gizclaw_debug_snapshot {
  /** A refresh or set completed successfully at least once. */
  bool known;
  /** Confirmed server mode; empty when the server holds none. */
  char mode[64];
  /** A refresh or set is in flight. */
  bool busy;
  /** Result of the last completed refresh or set. */
  h2_pal_result_t last_result;
  /** Increments on every completed refresh or set. */
  uint32_t revision;
} h2_gizclaw_debug_snapshot_t;

/** Copy the current snapshot. Never blocks on the network. */
h2_pal_result_t h2_gizclaw_debug_snapshot(h2_gizclaw_service_t *service,
                                          h2_gizclaw_debug_snapshot_t *out);
/** Start reading the server mode (server.runtime.get). BUSY while another
 * refresh or set is in flight. The result is folded into the snapshot the
 * next time snapshot/refresh/set_mode is called after the request finished;
 * nothing here waits for the network. */
h2_pal_result_t h2_gizclaw_debug_refresh(h2_gizclaw_service_t *service,
                                         uint32_t timeout_ms);
/** Start changing the server mode (server.runtime.put). The snapshot only
 * adopts `mode` after the server confirms it. BUSY while in flight. */
h2_pal_result_t h2_gizclaw_debug_set_mode(h2_gizclaw_service_t *service,
                                          h2_gizclaw_str_t mode,
                                          uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
