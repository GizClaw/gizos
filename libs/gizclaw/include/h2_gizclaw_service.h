#ifndef H2_GIZCLAW_SERVICE_H
#define H2_GIZCLAW_SERVICE_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2_gizclaw_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_service h2_gizclaw_service_t;
typedef struct h2_gizclaw_operation h2_gizclaw_operation_t;
typedef struct h2_gizclaw_cancel_token h2_gizclaw_cancel_token_t;
typedef struct h2_gizclaw_async_rpc h2_gizclaw_async_rpc_t;
typedef struct h2_gizclaw_async_stream h2_gizclaw_async_stream_t;

/** Terminal state delivered exactly once for every accepted operation. */
typedef enum h2_gizclaw_operation_terminal_kind {
  H2_GIZCLAW_OPERATION_FINISHED = 0,
  H2_GIZCLAW_OPERATION_CANCELED = 1,
  H2_GIZCLAW_OPERATION_SERVICE_CLOSED = 2,
} h2_gizclaw_operation_terminal_kind_t;

/** Immutable operation result visible during response dispatch. */
typedef struct h2_gizclaw_operation_result {
  uint64_t identity;
  h2_gizclaw_operation_terminal_kind_t terminal_kind;
  h2_pal_result_t result;
} h2_gizclaw_operation_result_t;

/**
 * Execute one network operation on the sole client-owning network task.
 *
 * `user` remains caller-owned through completion callback return. The run
 * function is invoked exactly once for an accepted, non-queued-canceled
 * operation and returns that operation's result. It must cooperate with the
 * supplied cancellation token and must not retain the client or token.
 */
typedef h2_pal_result_t (*h2_gizclaw_operation_run_fn)(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token);

/** Execute one synchronous progress callback on the response-dispatch task. */
typedef h2_pal_result_t (*h2_gizclaw_operation_dispatch_fn)(void *user);

/**
 * Apply one terminal result on the service-owned response-dispatch task.
 *
 * The operation, result view, and caller-owned `user` remain valid through
 * callback return. This callback may release its operation handle.
 */
typedef void (*h2_gizclaw_operation_completion_fn)(
    void *user, h2_gizclaw_operation_t *operation,
    const h2_gizclaw_operation_result_t *result);

/** Completion for one service-owned asynchronous unary RPC. */
typedef void (*h2_gizclaw_async_rpc_completion_fn)(
    void *user, h2_gizclaw_async_rpc_t *rpc,
    const h2_gizclaw_operation_result_t *result,
    const h2_gizclaw_rpc_response_t *response);

/** One mixed-frame event delivered on `$gizclaw/resp_dispatch`. */
typedef h2_pal_result_t (*h2_gizclaw_async_stream_event_fn)(
    void *user, h2_gizclaw_async_stream_t *stream,
    const h2_gizclaw_rpc_stream_event_t *event);

/** Terminal completion for one service-owned mixed-frame RPC. */
typedef void (*h2_gizclaw_async_stream_completion_fn)(
    void *user, h2_gizclaw_async_stream_t *stream,
    const h2_gizclaw_operation_result_t *result,
    const h2_gizclaw_rpc_response_t *response);

/**
 * Report one fatal connection failure on the response-dispatch task.
 *
 * Dispatch invokes this callback exactly once after all affected operation
 * callbacks. Explicit service stop does not invoke it.
 */
typedef void (*h2_gizclaw_service_terminal_fn)(void *user,
                                               h2_pal_result_t result);

/** Run caller-owned preparation on the service worker before client creation.
 */
typedef h2_pal_result_t (*h2_gizclaw_service_prepare_fn)(
    void *user, h2_gizclaw_cancel_fn cancel_requested, void *cancel_user);

/** Release caller-owned client resources on the worker before client close. */
typedef void (*h2_gizclaw_service_cleanup_fn)(void *user,
                                              h2_gizclaw_client_t *client);

typedef struct h2_gizclaw_service_config {
  /** Borrowed until `h2_gizclaw_service_deinit()` returns. */
  const h2_gizclaw_config_t *client_config;
  /** Borrowed PAL APIs; their providers outlive the service. */
  const h2_pal_task_api_t *task;
  const h2_pal_queue_api_t *queue;
  const h2_pal_sync_api_t *sync;
  /** Stack requirements for the system-owned `$gizclaw/net` task. */
  h2_pal_task_options_t net_task_options;
  /** Stack requirements for the system-owned `$gizclaw/resp_dispatch` task. */
  h2_pal_task_options_t resp_dispatch_task_options;
  /** Maximum admitted operations across all lifecycle states. */
  size_t operation_capacity;
  /** Worker poll/receive bound in milliseconds; must be positive. */
  int client_poll_timeout_ms;
  /** Optional Peer Event handler, invoked on response dispatch. */
  h2_gizclaw_client_event_fn on_event;
  void *event_user;
  /** Optional worker-side preparation performed before client creation. */
  h2_gizclaw_service_prepare_fn prepare;
  void *prepare_user;
  /** Optional worker-side cleanup performed before client close/deinit. */
  h2_gizclaw_service_cleanup_fn cleanup;
  void *cleanup_user;
  h2_gizclaw_service_terminal_fn terminal;
  void *terminal_user;
} h2_gizclaw_service_config_t;

/** Return whether service stop, caller cancellation, or client cancellation is
 * requested. */
bool h2_gizclaw_cancel_requested(const h2_gizclaw_cancel_token_t *cancel_token);

/**
 * Hand one bounded progress step from the network task to response dispatch.
 *
 * Only the accepted operation's worker callback may call this function, and it
 * must not call it recursively. `callback` runs synchronously on the caller of
 * the service-owned response-dispatch task without the service mutex held. The
 * `user` context remains worker-owned and must remain valid until this function
 * returns; the progress callback must not retain it.
 *
 * One completion-queue slot is reserved while the network task blocks. Response
 * dispatch atomically claims an unstarted callback under the service mutex.
 * Cancellation or stop before that claim skips the callback and reuses the
 * reserved capacity for exactly one terminal completion. Cancellation after
 * the claim does not interrupt the callback; the worker remains blocked until
 * it returns and then completes as canceled or service-closed. The callback's
 * PAL result is returned unchanged only when no cancellation or stop wins
 * first.
 *
 * This is intended for an exclusive streaming operation that must ask the App
 * main loop to perform product-owned Audio or state work between client I/O
 * steps.
 */
h2_pal_result_t h2_gizclaw_operation_dispatch_call(
    const h2_gizclaw_cancel_token_t *cancel_token,
    h2_gizclaw_operation_dispatch_fn callback, void *user);

/**
 * Allocate queues and synchronization state without starting tasks.
 *
 * The returned service is caller-owned until successful deinit. Init, start,
 * stop and deinit belong to one lifecycle task.
 */
h2_pal_result_t
h2_gizclaw_service_init(const h2_gizclaw_service_config_t *config,
                        h2_gizclaw_service_t **out_service);

/** Start the client-owning network and response-dispatch tasks exactly once. */
h2_pal_result_t h2_gizclaw_service_start(h2_gizclaw_service_t *service);

/**
 * Admit one FIFO operation without blocking.
 *
 * This API is task-safe and non-blocking. A successful call returns one
 * caller-owned handle reference and borrows `user` through completion callback
 * return. A failed call returns no handle, never invokes either callback, and
 * does not consume `user`.
 */
h2_pal_result_t
h2_gizclaw_service_submit(h2_gizclaw_service_t *service, uint64_t identity,
                          h2_gizclaw_operation_run_fn run,
                          h2_gizclaw_operation_completion_fn completion,
                          void *user, h2_gizclaw_operation_t **out_operation);

/**
 * Submit one task-safe unary RPC to the service's sole client owner.
 *
 * The encoded payload is copied before return. The response is owned by the
 * returned handle and borrowed by completion until the handle is released.
 */
h2_pal_result_t h2_gizclaw_service_rpc_call_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t params_payload,
    uint32_t timeout_ms, h2_gizclaw_async_rpc_completion_fn completion,
    void *user, h2_gizclaw_async_rpc_t **out_rpc);

/** Request task-safe, idempotent cancellation of an asynchronous RPC. */
h2_pal_result_t h2_gizclaw_async_rpc_cancel(h2_gizclaw_async_rpc_t *rpc);

/** Release one terminal RPC handle. Calls before completion are ignored. */
void h2_gizclaw_async_rpc_release(h2_gizclaw_async_rpc_t *rpc);

/** Submit one server-streaming RPC driven exclusively by `$gizclaw/net`. */
h2_pal_result_t h2_gizclaw_service_rpc_stream_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t params_payload,
    uint32_t timeout_ms, h2_gizclaw_async_stream_event_fn on_event,
    h2_gizclaw_async_stream_completion_fn completion, void *user,
    h2_gizclaw_async_stream_t **out_stream);

h2_pal_result_t
h2_gizclaw_async_stream_cancel(h2_gizclaw_async_stream_t *stream);

void h2_gizclaw_async_stream_release(h2_gizclaw_async_stream_t *stream);

/** Request task-safe, idempotent cooperative cancellation without blocking. */
h2_pal_result_t h2_gizclaw_operation_cancel(h2_gizclaw_operation_t *operation);

/** Release the caller-owned operation handle exactly once; this is task-safe.
 */
void h2_gizclaw_operation_release(h2_gizclaw_operation_t *operation);

/**
 * Stop and join both tasks after all accepted callbacks have been dispatched.
 *
 * This lifecycle-task API is idempotent and must not be called from a service
 * callback.
 */
h2_pal_result_t h2_gizclaw_service_stop(h2_gizclaw_service_t *service);

/**
 * Release a stopped, fully dispatched service with no caller-owned handles.
 *
 * This lifecycle-task API must not race submit, cancel, or release.
 */
h2_pal_result_t h2_gizclaw_service_deinit(h2_gizclaw_service_t *service);

#ifdef __cplusplus
}
#endif

#endif
