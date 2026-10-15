#ifndef H2_WEB_GIZCLAW_H
#define H2_WEB_GIZCLAW_H
#include "h2/pal/os/h2_pal_queue.h"
#include "h2_gizclaw_device.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_web_gizclaw h2_web_gizclaw_t;
/** Dependencies are borrowed until successful close. device is the product's
 * already-started standalone context, also borrowed by its network Service.
 * Zero selects four queued calls, 64 KiB per request and 30000 ms per call.
 * Capacity is at most 32, byte limit at most 1 MiB, timeout at most 120000 ms.
 */
typedef struct h2_web_gizclaw_config {
  h2_gizclaw_device_t *device;
  const h2_pal_mem_api_t *allocator;
  const h2_pal_queue_api_t *queue;
  const h2_pal_task_api_t *task;
  size_t request_capacity;
  size_t request_limit_bytes;
  uint32_t timeout_ms;
} h2_web_gizclaw_config_t;
/** Bind globalThis.h2WebGizClaw from a Worker. mhs.read/write take ordinary
 * objects; tools.list/invoke use the canonical SDK registry/codec. call takes
 * canonical method/payload bytes. All operations return Promises, accept
 * {signal?, timeout_ms?}, and preserve signed PAL error.code plus rpcCode and
 * rpcMessage. False/zero optional values retain their presence. No remote
 * connection or Ready state is needed. Only one bridge attaches per module.
 * Requests copy their input at admission; saturation returns BUSY. A timeout
 * or cancellation does not undo an executing hardware write: re-read state.
 *
 * The bridge retains a consumer reference to device. Stop/destroy refuse
 * until close actually joins and releases it. Failure may leave a retained
 * non-NULL bridge; retry close before releasing any dependency. */
h2_pal_result_t h2_web_gizclaw_open(const h2_web_gizclaw_config_t *config,
                                    h2_web_gizclaw_t **out_bridge);
/** Close JS ingress, reject waiters, cancel active calls and join the adapter
 * Worker before release. Called on a Worker, never the UI. NULL is a no-op.
 * Failure keeps the pointer and dependencies alive; close can be retried.
 * Then stop/deinit Service and stop device before handing Runtime to Lua.
 * The separate catalog installer bridge may remain active during Lua. */
h2_pal_result_t h2_web_gizclaw_close(h2_web_gizclaw_t **bridge);
#ifdef __cplusplus
}
#endif
#endif
