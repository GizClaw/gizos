#ifndef H2_WEB_GIZCLAW_H
#define H2_WEB_GIZCLAW_H
#include "h2/pal/os/h2_pal_queue.h"
#include "h2_gizclaw_local.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_web_gizclaw h2_web_gizclaw_t;
/** Dependencies are borrowed through successful close. service has already
 * enabled h2_gizclaw_local_start(); both local and WebRTC ingress use its
 * existing handlers. Zero selects four calls, 64 KiB and 30000 ms.
 * Capacity <=32, byte limit <=1 MiB and timeout <=120000 ms. */
typedef struct h2_web_gizclaw_config {
  h2_gizclaw_service_t *service;
  const h2_pal_mem_api_t *allocator;
  const h2_pal_queue_api_t *queue;
  const h2_pal_task_api_t *task;
  size_t request_capacity;
  size_t request_limit_bytes;
  uint32_t timeout_ms;
} h2_web_gizclaw_config_t;
/** Register the versioned iframe transport from a Worker. Standalone firmware
 * runs without an npm SDK. Only a parent page that controls this firmware
 * imports @gizclaw/firmware and calls connectFirmware(iframe).
 * The parent-origin/window handshake creates a MessageChannel. Canonical
 * request bytes use the existing local Service; no object codec or controller
 * API is embedded in the firmware. Replies remain borrowed until the parent
 * SDK acknowledges copying/decoding; cancellation cannot undo executed writes.
 * A new parent connection replaces the old connection. Different frames are
 * independent. The adapter retains the Service until successful close.
 * Open failure can retain a non-NULL handle if cleanup fails; retry close.
 */
h2_pal_result_t h2_web_gizclaw_open(const h2_web_gizclaw_config_t *,
                                   h2_web_gizclaw_t **);
/** Detach the endpoint, close the controller connection, cancel requests and join the adapter
 * worker before releasing its borrow. Worker-only; NULL *bridge is a no-op.
 * Failure retains the pointer/dependencies for retry. Close this adapter before
 * Service stop/deinit. Existing tool continuations and Lua lifecycles are
 * owned by the existing product hooks; this adapter adds none. */
h2_pal_result_t h2_web_gizclaw_close(h2_web_gizclaw_t **bridge);
#ifdef __cplusplus
}
#endif
#endif
