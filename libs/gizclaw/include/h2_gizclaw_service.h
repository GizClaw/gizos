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

/** Terminal state delivered exactly once for every accepted operation. */
typedef enum h2_gizclaw_operation_terminal_kind {
  H2_GIZCLAW_OPERATION_FINISHED = 0,
  H2_GIZCLAW_OPERATION_CANCELED = 1,
  H2_GIZCLAW_OPERATION_SERVICE_CLOSED = 2,
} h2_gizclaw_operation_terminal_kind_t;

/** Immutable operation result visible during caller-thread dispatch. */
typedef struct h2_gizclaw_operation_result {
  uint64_t identity;
  h2_gizclaw_operation_terminal_kind_t terminal_kind;
  h2_pal_result_t result;
} h2_gizclaw_operation_result_t;

/**
 * Execute one synchronous typed request on the sole service worker.
 *
 * `user` remains caller-owned through completion callback return. The run
 * function is invoked exactly once for an accepted, non-queued-canceled
 * operation and returns that operation's result. It must cooperate with the
 * supplied cancellation token and must not retain the client or token.
 */
typedef h2_pal_result_t (*h2_gizclaw_operation_run_fn)(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token);

/** Execute one synchronous progress callback on the dispatch caller thread. */
typedef h2_pal_result_t (*h2_gizclaw_operation_dispatch_fn)(void *user);

/**
 * Apply one terminal result on the thread calling service dispatch.
 *
 * The operation, result view, and caller-owned `user` remain valid through
 * callback return. This callback may release its operation handle.
 */
typedef void (*h2_gizclaw_operation_completion_fn)(
    void *user, h2_gizclaw_operation_t *operation,
    const h2_gizclaw_operation_result_t *result);

/**
 * Report one fatal connection failure on the dispatch caller thread.
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
  /** Stack requirements for the system-owned `$gizclaw/service` task. */
  h2_pal_task_options_t task_options;
  /** Maximum admitted operations across all lifecycle states. */
  size_t operation_capacity;
  /** Worker poll/receive bound in milliseconds; must be positive. */
  int client_poll_timeout_ms;
  /** Optional connection-scoped Peer Event handler, invoked on the worker. */
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
 * Hand one bounded progress step from the worker to the dispatch caller.
 *
 * Only the accepted operation's worker callback may call this function, and it
 * must not call it recursively. `callback` runs synchronously on the caller of
 * `h2_gizclaw_service_dispatch()` without the service mutex held. The `user`
 * context remains worker-owned and must remain valid until this function
 * returns; the progress callback must not retain it.
 *
 * One completion-queue slot is reserved while the worker blocks. Dispatch
 * atomically claims an unstarted callback under the service mutex. Cancellation
 * or stop before that claim skips the callback and reuses the reserved capacity
 * for exactly one terminal completion. Cancellation after the claim does not
 * interrupt the callback; the worker remains blocked until it returns and then
 * completes as canceled or service-closed. The callback's PAL result is
 * returned unchanged only when no cancellation or stop wins first.
 *
 * This is intended for an exclusive streaming operation that must ask the App
 * main loop to perform product-owned Audio or state work between client I/O
 * steps.
 */
h2_pal_result_t h2_gizclaw_operation_dispatch_call(
    const h2_gizclaw_cancel_token_t *cancel_token,
    h2_gizclaw_operation_dispatch_fn callback, void *user);

/**
 * Allocate queues and synchronization state without starting a task.
 *
 * The returned service is caller-owned until successful deinit. Init, start,
 * stop, dispatch, and deinit belong to one lifecycle task.
 */
h2_pal_result_t
h2_gizclaw_service_init(const h2_gizclaw_service_config_t *config,
                        h2_gizclaw_service_t **out_service);

/** Start the service's sole client-owning worker task exactly once. */
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

/** Request task-safe, idempotent cooperative cancellation without blocking. */
h2_pal_result_t h2_gizclaw_operation_cancel(h2_gizclaw_operation_t *operation);

/** Release the caller-owned operation handle exactly once; this is task-safe.
 */
void h2_gizclaw_operation_release(h2_gizclaw_operation_t *operation);

/**
 * Invoke at most `max_callbacks` completions on the calling task.
 *
 * This API has one consumer and must not be called recursively. A completion
 * may submit work, cancel another operation, or release an operation handle;
 * it must not call dispatch, stop, or deinit.
 */
h2_pal_result_t h2_gizclaw_service_dispatch(h2_gizclaw_service_t *service,
                                            size_t max_callbacks,
                                            size_t *out_dispatched);

/**
 * Stop and join the worker without invoking completion callbacks.
 *
 * This lifecycle-task API is idempotent and must not be called from a service
 * completion callback. Accepted callbacks remain pending for dispatch.
 */
h2_pal_result_t h2_gizclaw_service_stop(h2_gizclaw_service_t *service);

/**
 * Release a stopped, fully dispatched service with no caller-owned handles.
 *
 * This lifecycle-task API must not race submit, cancel, release, or dispatch.
 */
h2_pal_result_t h2_gizclaw_service_deinit(h2_gizclaw_service_t *service);

#ifdef __cplusplus
}
#endif

#endif
