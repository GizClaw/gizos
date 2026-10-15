#ifndef H2_GIZCLAW_DEVICE_H
#define H2_GIZCLAW_DEVICE_H

#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2_gizclaw_config.h"
#include "h2_gizclaw_rpc.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_device h2_gizclaw_device_t;
typedef struct h2_gizclaw_device_call h2_gizclaw_device_call_t;

/** Product config is copied; its pointed-to PALs, registrations, catalog and
 * hook userdata remain borrowed through successful device destruction.
 * No endpoint, key, signaling connection or remote Ready state is required
 * for local operations. Network-dependent tools retain their ordinary errors.
 */
typedef struct h2_gizclaw_device_config {
  const h2_gizclaw_config_t *client_config;
  const h2_pal_task_api_t *task;
  const h2_pal_sync_api_t *sync;
  h2_runtime_t *runtime;
  /** Zero selects four admitted calls and 64 KiB per copied request. */
  size_t request_capacity;
  size_t request_limit_bytes;
} h2_gizclaw_device_config_t;

/** Actual PAL execution result and canonical RPC response view. Byte spans
 * remain borrowed until call_consume() or call_release().
 * A wire rejection has has_error set, independently of the PAL result.
 */
typedef struct h2_gizclaw_device_result {
  h2_pal_result_t result;
  h2_gizclaw_rpc_bytes_t payload;
  bool has_error;
  int error_code;
  h2_gizclaw_rpc_bytes_t error_message;
} h2_gizclaw_device_result_t;

/** Create a local provider without a network endpoint or connection. The
 * config is copied; pointed-to PALs, product userdata, registrations and
 * Runtime remain borrowed until checked destroy clears the owner pointer.
 * Failure may retain a non-NULL handle if rollback cleanup fails; retry
 * destroy and keep its dependencies alive. No operation is valid on a
 * partially created handle except destroy. Lifecycle calls are serial. */
h2_pal_result_t
h2_gizclaw_device_create(const h2_gizclaw_device_config_t *config,
                         h2_gizclaw_device_t **out_device);
/** Start the shared local RPC owner and existing device work. Restart is
 * allowed only after a successful stop and no retained calls or attachment.
 */
h2_pal_result_t h2_gizclaw_device_start(h2_gizclaw_device_t *device);
/** Close admission, cancel pending work and stop/join device tasks. This may
 * block until a synchronous product callback returns. A retained call,
 * consumer reference or failed join returns an error and keeps dependencies
 * alive. Fatal owner synchronization faults also retain state for backend or
 * process recovery. Stop/deinit an attached Service first.
 */
h2_pal_result_t h2_gizclaw_device_stop(h2_gizclaw_device_t *device);
/** Checked release of a stopped, detached device with no outstanding calls.
 * NULL *device succeeds; failure retains *device and all dependencies.
 */
h2_pal_result_t h2_gizclaw_device_destroy(h2_gizclaw_device_t **device);
/** Retain a consumer borrow (e.g. the Web bridge). Stop and destroy return BUSY
 * while it exists. Retain and new consumer creation are serial with destroy. */
h2_pal_result_t h2_gizclaw_device_retain(h2_gizclaw_device_t *device);
/** Release a consumer borrow and clear its pointer. This never destroys the
 * owner's device. Failure leaves the pointer and reference intact. */
h2_pal_result_t h2_gizclaw_device_release(h2_gizclaw_device_t **device);

/** Admit a canonical local RPC (MHS 133/134, tool 135/136, methods 137).
 * Unknown numeric methods are accepted and answered UNIMPLEMENTED by the SDK.
 * Invoke uses the canonical outer request and response envelopes.
 * timeout_ms must be 1..120000 and bounds queue/result delivery; a synchronous
 * product callback can outlive it, retaining state until it actually returns.
 * Request bytes are copied. Rejection accepts no work and leaves out NULL.
 * Callback, validation and response completion share one RPC owner with the
 * network Service. Callbacks must not reenter blocking device lifecycle APIs or
 * call_wait().
 */
h2_pal_result_t h2_gizclaw_device_submit(h2_gizclaw_device_t *device,
                                         h2_gizclaw_rpc_method_t method,
                                         h2_gizclaw_rpc_bytes_t request,
                                         uint32_t timeout_ms,
                                         h2_gizclaw_device_call_t **out_call);
/** Wait for a terminal execution view. A wait timeout does not consume the
 * handle or prove a hardware write was canceled; re-read hardware if needed.
 */
h2_pal_result_t
h2_gizclaw_device_call_wait(h2_gizclaw_device_call_t *call, uint32_t timeout_ms,
                            h2_gizclaw_device_result_t *out_result);
/** Accept cancellation before the owner selects completion. OK accepts the
 * cancellation; CLOSED means completion already started or finished. An
 * executing hardware operation may still apply changes. One caller owns each
 * handle: serialize wait/consume/complete/release. Cancel may run concurrently
 * while that caller reference remains owned. */
h2_pal_result_t h2_gizclaw_device_call_cancel(h2_gizclaw_device_call_t *call);
/** Relinquish byte spans while retaining a later delivery-completion token.
 * Frees copied payload/error bytes; it does not deliver completion. Network
 * adapters use it after synchronous response copy while retaining the token
 * through the transport close handshake. Each call has independent storage.
 */
h2_pal_result_t h2_gizclaw_device_call_consume(h2_gizclaw_device_call_t *call);
/** Queue exactly one delivery completion. Product continuations
 * run on the shared owner; errors cancel a prepared action. JS uses OK only
 * after copying and decoding the result into caller-owned values.
 */
h2_pal_result_t
h2_gizclaw_device_call_complete(h2_gizclaw_device_call_t *call,
                                h2_pal_result_t delivery_result);
/** Relinquish the caller handle. An unfinished call is canceled; owner state
 * remains retained until execution and completion have actually quiesced.
 */
h2_pal_result_t h2_gizclaw_device_call_release(h2_gizclaw_device_call_t **call);

#ifdef __cplusplus
}
#endif
#endif
