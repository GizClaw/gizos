#ifndef H2_GIZCLAW_SERVICE_H
#define H2_GIZCLAW_SERVICE_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2_runtime_custom_event.h"
#include "h2_gizclaw_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_service h2_gizclaw_service_t;
typedef struct h2_gizclaw_req h2_gizclaw_req_t;
typedef struct h2_gizclaw_track h2_gizclaw_track_t;

/** A valid RPC error response, distinct from PAL transport/format failures.
 * A canonical NOT_FOUND status instead returns H2_PAL_ERR_NOT_FOUND from
 * req_wait, response parsers and synchronous RPCs. UNIMPLEMENTED and all other
 * status codes remain H2_GIZCLAW_ERR_REMOTE. */
#define H2_GIZCLAW_ERR_REMOTE ((h2_pal_result_t) - 1000)

/** Fill at most capacity bytes for one data-up request. `OK` with zero bytes
 * closes the request input; `WOULD_BLOCK` keeps it active for a later retry. */
typedef h2_pal_result_t (*h2_gizclaw_req_input_read_fn)(
    void *user, uint8_t *buffer, size_t capacity, size_t *out_read);

/** Consume data-down bytes on the task calling service_poll(). Partial writes
 * and `WOULD_BLOCK` retain the remaining bytes for a later poll. */
typedef h2_pal_result_t (*h2_gizclaw_req_output_write_fn)(
    void *user, const uint8_t *data, size_t length, size_t *out_written);

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

/** Optional terminal hook dispatched at most once by service_poll(). The
 * request and result are borrowed for the duration of the call. Waiting for
 * request completion does not wait for this hook. If the bounded dispatch
 * queue is full, the request ends with WOULD_BLOCK and the hook is dropped. */
typedef void (*h2_gizclaw_req_complete_fn)(
    void *user, h2_gizclaw_req_t *request,
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
  /** Borrowed until `h2_gizclaw_service_deinit()` returns. Requires a
   * monotonic-ms time provider for the 20 ms audio schedule. */
  const h2_gizclaw_config_t *client_config;
  /** Borrowed PAL APIs; their providers outlive the service. */
  const h2_pal_task_api_t *task;
  const h2_pal_queue_api_t *queue;
  const h2_pal_sync_api_t *sync;
  /** Optional Runtime to wake. When set, the Service calls
   * h2_runtime_notify() whenever caller-thread work becomes available; the
   * Runtime coalesces those wakes. The Runtime must outlive the Service. */
  h2_runtime_t *runtime;
  /** Stack requirements for the system-owned `$gizclaw/net` task. */
  h2_pal_task_options_t net_task_options;
  /** Maximum admitted operations across all lifecycle states. */
  size_t operation_capacity;
  /** Worker poll/receive bound in milliseconds; must be positive. */
  int client_poll_timeout_ms;
  /** Optional Peer Event handler, invoked during caller-thread dispatch. */
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

/**
 * Allocate queues and synchronization state without starting a task.
 *
 * The returned service is caller-owned until successful deinit. Init, start,
 * stop, poll and deinit belong to one lifecycle task.
 */
h2_pal_result_t
h2_gizclaw_service_init(const h2_gizclaw_service_config_t *config,
                        h2_gizclaw_service_t **out_service);

/** Start the network, PCM audio-up/audio-down and data-up/data-down tasks
 * once. Each direction owns at most one active request; another request for the
 * same direction returns BUSY instead of being queued. Opposite directions
 * remain independent. Partial startup failure attempts to stop already-started
 * tasks and returns the original start error. Call stop to finish cleanup
 * before deinit. */
h2_pal_result_t h2_gizclaw_service_start(h2_gizclaw_service_t *service);

/** Bind one library-created PCM Track until unset succeeds. Applications use
 * pcm_track_write/read for their mic/speaker pumps, not a callback vtable.
 * Rebinding is rejected while the previous audio route is still active. */
h2_pal_result_t h2_gizclaw_service_set_track(h2_gizclaw_service_t *service,
                                             h2_gizclaw_track_t *track);
/** Stop admitting Track accesses and wait for in-flight reads/writes. Active
 * audio requests observe CLOSED on their next Track access. */
h2_pal_result_t h2_gizclaw_service_unset_track(h2_gizclaw_service_t *service,
                                               h2_gizclaw_track_t *track);

/** Start the currently registered ASR or Conversation audio route. Requests
 * alone never consume mic PCM. Old buffered PCM is discarded by uplink. */
h2_pal_result_t h2_gizclaw_service_audio_start(h2_gizclaw_service_t *service);

/** End the current recording, without a request handle or waiting for upload.
 * Uplink copies the frozen PCM tail before releasing its read cursor, then
 * sends the tail and protocol EOS. The mic producer continues independently.
 * Repeated end is harmless; calling before audio_start returns INVALID_STATE.
 */
h2_pal_result_t h2_gizclaw_service_audio_end(h2_gizclaw_service_t *service);

/** Unified request lifecycle.
 *
 * Create functions only retain protocol parameters. `do` starts an accepted
 * request once and binds its optional callbacks. The input callback runs on
 * data-up; the output and completion callbacks run only from service_poll().
 * A missing fixed-length input callback produces the requested benchmark
 * bytes without copying payload; a missing output callback validates and
 * consumes the response without dispatching its data to the App.
 * `user` must remain valid until the completion hook returns, or until
 * req_wait returns when no completion hook is supplied.
 * A failed do does not consume the caller's request reference.
 */
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete);

/** Wait for execution terminal state and return its result, without polling
 * or dispatching callbacks. A timeout neither cancels nor finishes input.
 * Repeated/concurrent waits observe the same terminal result. */
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request,
                                    uint32_t timeout_ms);
/** Idempotently cancel the request; this is not an input half-close. */
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request);
/** Drop the caller reference, without canceling. Execution and queued callbacks
 * hold internal references. The handle must not be used after this call. */
void h2_gizclaw_req_release(h2_gizclaw_req_t *request);

/**
 * Invoke at most `max_callbacks` callbacks on the calling task.
 * The connection-terminal hook shares this budget and is included in
 * `out_dispatched`; when the budget is exhausted it waits for a later poll.
 *
 * When config.runtime is set, call this after every h2_runtime_wait_notify()
 * return (the Service wakes the Runtime without posting an event). A wake
 * means "dispatch work may exist", not "one callback exists". This function
 * wakes the Runtime again when its callback budget leaves work pending.
 *
 * This API has one consumer and must not be called recursively. A callback may
 * create/submit requests, cancel or release handles, and stop the service. It
 * must not recursively call poll or deinit. Pending callbacks remain valid
 * after stop and are drained by poll.
 */
h2_pal_result_t h2_gizclaw_service_poll(h2_gizclaw_service_t *service,
                                        size_t max_callbacks,
                                        size_t *out_dispatched);

/**
 * Stop and join the network, uplink and downlink tasks without invoking pending
 * callbacks.
 *
 * This lifecycle-task API is idempotent, including from a service_poll hook.
 * Do not call it from a worker-side prepare/cleanup hook.
 * Accepted callbacks remain pending for caller-thread dispatch.
 * If a task join fails, its handle is retained; retry stop before deinit.
 * Tasks already joined successfully are not joined again.
 */
h2_pal_result_t h2_gizclaw_service_stop(h2_gizclaw_service_t *service);

/**
 * Release a stopped, fully dispatched service with no caller-owned handles.
 *
 * This lifecycle-task API must not race submit, cancel, release or dispatch.
 */
h2_pal_result_t h2_gizclaw_service_deinit(h2_gizclaw_service_t *service);

#ifdef __cplusplus
}
#endif

#endif
