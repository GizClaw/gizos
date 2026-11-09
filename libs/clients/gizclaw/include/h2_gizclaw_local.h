#ifndef H2_GIZCLAW_LOCAL_H
#define H2_GIZCLAW_LOCAL_H
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_rpc.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_local_call h2_gizclaw_local_call_t;
/** Execution result plus canonical response bytes. Views remain borrowed until
 * consume/release; wire rejections are independent of signed PAL errors. */
typedef struct h2_gizclaw_local_result {
  h2_pal_result_t result;
  h2_gizclaw_rpc_bytes_t payload;
  bool has_error;
  int error_code;
  h2_gizclaw_rpc_bytes_t error_message;
} h2_gizclaw_local_result_t;

/** Enable local ingress on an initialized Service, before service_start().
 * Starts the existing device worker and a bounded dispatch owner; no network
 * connection or remote Ready is required. WebRTC subsequently uses the same
 * registered tool/MHS callbacks and serial owner. Lua installation/run and
 * every other tool keep their existing implementations and config.
 * Lifecycle calls are serial. Failure retains cleanup state when needed;
 * stop/deinit the Service and rebuild it instead of starting networking.
 * Stop closes both ingresses, cancels and joins; deinit refuses retained
 * calls/borrowers. Default Services that omit this call keep existing behavior.
 */
h2_pal_result_t h2_gizclaw_local_start(h2_gizclaw_service_t* service);
/** Borrow for an adapter. Keep PALs/config alive until Service deinit succeeds.
 * Retain/adapter creation is serial with lifecycle cleanup. */
h2_pal_result_t h2_gizclaw_local_retain(h2_gizclaw_service_t* service);
/** Release only this borrow and clear its pointer. Failure retains it. */
h2_pal_result_t h2_gizclaw_local_release(h2_gizclaw_service_t** service);
/** Copy/admit a canonical request (MHS read/write, tool invoke/list, methods
 * list). Tool invoke uses the SDK's outer envelope. Four outstanding calls,
 * each at most 64 KiB. Rejection accepts no work and clears out_call.
 * timeout_ms is 1..120000 and bounds queue/result delivery, not rollback of
 * side effects. Synchronous callbacks can outlive cancellation/deadlines.
 * Callbacks and response completion run on the shared owner; do not reenter
 * blocking lifecycle/wait calls. No Lua or other tool logic is duplicated.
 */
h2_pal_result_t h2_gizclaw_local_submit(h2_gizclaw_service_t* service,
                                        h2_gizclaw_rpc_method_t method,
                                        h2_gizclaw_rpc_bytes_t request,
                                        uint32_t timeout_ms,
                                        h2_gizclaw_local_call_t** out_call);
/** Wait without consuming the handle. Timeout does not prove a write canceled.
 * One caller serializes wait/consume/complete/release for each handle. */
h2_pal_result_t h2_gizclaw_local_call_wait(
    h2_gizclaw_local_call_t* call, uint32_t timeout_ms,
    h2_gizclaw_local_result_t* out_result);
/** Cancel before completion selection; CLOSED means completion has begun.
 * May run concurrently while the caller still owns its reference. */
h2_pal_result_t h2_gizclaw_local_call_cancel(h2_gizclaw_local_call_t* call);
/** Release response byte views while keeping the delivery-completion token. */
h2_pal_result_t h2_gizclaw_local_call_consume(h2_gizclaw_local_call_t* call);
/** Select delivery once, after copying/decoding. OK enables an existing
 * prepared tool continuation; any failure cancels it. */
h2_pal_result_t h2_gizclaw_local_call_complete(h2_gizclaw_local_call_t* call,
                                               h2_pal_result_t delivery_result);
/** Clear the owned caller handle. Unfinished work is canceled; owner state is
 * retained until execution and completion actually finish. NULL is a no-op. */
h2_pal_result_t h2_gizclaw_local_call_release(h2_gizclaw_local_call_t** call);
#ifdef __cplusplus
}
#endif
#endif
