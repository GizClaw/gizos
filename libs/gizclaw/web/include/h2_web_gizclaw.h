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
/** Bind globalThis.h2WebGizClaw from a Worker. mhs.read/write and tools.list/
 * invoke use ordinary objects and the canonical SDK codec/registry. call uses
 * canonical method/payload bytes. All return Promises and accept
 * {signal?, timeout_ms?}. Errors preserve signed PAL codes and actual wire
 * details; false/zero optional fields retain presence. No remote Ready is
 * needed. A timeout/abort cannot undo an executing write.
 * The adapter retains the Service's local ingress until successful close.
 * Open failure can retain a non-NULL handle if cleanup fails; retry close.
 */
h2_pal_result_t h2_web_gizclaw_open(const h2_web_gizclaw_config_t *,
                                   h2_web_gizclaw_t **);
/** Detach JS, reject pending Promises, cancel requests and join the adapter
 * worker before releasing its borrow. Worker-only; NULL *bridge is a no-op.
 * Failure retains the pointer/dependencies for retry. Close this adapter before
 * Service stop/deinit. Existing tool continuations and Lua lifecycles are
 * owned by the existing product hooks; this adapter adds none. */
h2_pal_result_t h2_web_gizclaw_close(h2_web_gizclaw_t **bridge);
#ifdef __cplusplus
}
#endif
#endif
