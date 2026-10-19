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
/** Register Module.h2GizClaw on this WASM runtime from a Worker. The public
 * version-1 byte API works without npm: request(method, Uint8Array, callback,
 * {timeout_ms?}) returns a positive call ID or a negative PAL admission error.
 * An accepted call invokes callback(response) exactly once on the UI thread,
 * asynchronously after request returns; cancel(id) completes it with CLOSED.
 * response contains code, hasError, rpcCode, rpcMessage, copied payload and
 * optional nativeTools. The synchronous callback returns a signed delivery
 * result (0 on successful consumption); throw/non-integer returns FORMAT.
 * No borrowed WASM pointers escape. Default limits are exposed on the API.
 * Timeout/cancel cannot undo executed writes. C close marks the API closed and
 * fails pending calls; old references reject requests with CLOSED.
 * Optional @gizclaw/firmware createFirmwareClient(Module) wraps this same byte
 * interface with canonical objects, types and Promises. Multiple clients may
 * share a runtime; client.close() cancels only its own calls. It does not stop
 * the Service. Product entries register through C and import no npm SDK.
 * The adapter retains the Service until successful close. Open failure can
 * retain a non-NULL handle if cleanup fails; retry close.
 */
h2_pal_result_t h2_web_gizclaw_open(const h2_web_gizclaw_config_t *,
                                   h2_web_gizclaw_t **);
/** Detach the runtime byte API, cancel requests and join the adapter
 * worker before releasing its borrow. Worker-only; NULL *bridge is a no-op.
 * Failure retains the pointer/dependencies for retry. Close this adapter before
 * Service stop/deinit. Existing tool continuations and Lua lifecycles are
 * owned by the existing product hooks; this adapter adds none. */
h2_pal_result_t h2_web_gizclaw_close(h2_web_gizclaw_t **bridge);
#ifdef __cplusplus
}
#endif
#endif
