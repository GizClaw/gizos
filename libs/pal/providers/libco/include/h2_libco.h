#ifndef H2_LIBCO_H
#define H2_LIBCO_H

#include <stddef.h>
#include <stdint.h>

#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reserved timeout value for a wait without a deadline. */
#define H2_LIBCO_WAIT_FOREVER UINT32_MAX
/** Wake every task currently waiting on the selected key. */
#define H2_LIBCO_WAKE_ALL SIZE_MAX
/** Default stack size used when task options specify zero bytes. */
#define H2_LIBCO_DEFAULT_STACK_SIZE (64u * 1024u)
/** Smallest accepted task stack. */
#define H2_LIBCO_MIN_STACK_SIZE 1024u
/** Required task stack-size and base-address alignment. */
#define H2_LIBCO_STACK_ALIGNMENT 16u

/** One single-threaded cooperative executor. */
typedef struct h2_libco h2_libco_t;
/** Opaque task handle retained until its owning executor is destroyed. */
typedef struct h2_libco_task h2_libco_task_t;

typedef enum h2_libco_result {
    H2_LIBCO_OK = 0,
    H2_LIBCO_WOKEN = 1,
    H2_LIBCO_ERR_INVALID_ARG = -1,
    H2_LIBCO_ERR_NO_MEMORY = -2,
    H2_LIBCO_ERR_BUSY = -3,
    H2_LIBCO_ERR_INVALID_STATE = -4,
    H2_LIBCO_ERR_TIMEOUT = -5,
    H2_LIBCO_ERR_CANCELLED = -6,
    H2_LIBCO_ERR_STACK_CORRUPT = -7,
    H2_LIBCO_ERR_EXTERNAL = -8,
} h2_libco_result_t;

/** Allocate @p size bytes or return NULL without retaining external state. */
typedef void *(*h2_libco_alloc_fn_t)(void *user, size_t size);
/** Release one non-NULL allocation returned by the matching allocator. */
typedef void (*h2_libco_free_fn_t)(void *user, void *memory);
/** Return monotonic milliseconds; wraparound is not permitted. */
typedef uint64_t (*h2_libco_now_ms_fn_t)(void *user);
/** Run a task and return its result; the entry may call libco task APIs. */
typedef int (*h2_libco_task_entry_fn_t)(void *user);
/** Import target-owned wake records before a scheduler turn. */
typedef h2_libco_result_t (*h2_libco_poll_external_fn_t)(
    void *user, h2_libco_t *core);
/** Idle the target until an external wake or the selected deadline. */
typedef void (*h2_libco_idle_fn_t)(
    void *user, int has_deadline, uint64_t deadline_ms);

typedef struct h2_libco_config {
    /** Opaque context forwarded to all injected callbacks. */
    void *user;
    /** Allocate executor metadata and task stacks. */
    h2_libco_alloc_fn_t alloc;
    /** Release memory returned by the matching allocator callback. */
    h2_libco_free_fn_t free;
    /** Read a monotonic millisecond clock used only for finite waits. */
    h2_libco_now_ms_fn_t now_ms;
    /** Optional borrowed wall-clock source; its sleep operation is ignored. */
    const h2_pal_time_api_t *time_source;
    /** Optional target wake-record import hook. */
    h2_libco_poll_external_fn_t poll_external;
    /** Optional target idle hook used only by an empty positive-budget turn. */
    h2_libco_idle_fn_t idle;
} h2_libco_config_t;

typedef struct h2_libco_task_options {
    /**
     * Aligned C-stack bytes, or zero for H2_LIBCO_DEFAULT_STACK_SIZE.
     *
     * Web builds allocate separate bounded Emscripten Fiber/Asyncify storage;
     * that private storage does not reduce this requested C-stack capacity.
     */
    size_t stack_size;
} h2_libco_task_options_t;

/**
 * Create an executor bound to the current thread and root context.
 *
 * The config is copied, but its callback context and services must remain valid
 * until destroy succeeds. APIs are single-threaded and not reentrant.
 *
 * @param config Borrowed callback configuration.
 * @param out_core Caller storage containing NULL; receives an owned executor.
 * @return OK, INVALID_ARG, or NO_MEMORY.
 */
h2_libco_result_t h2_libco_create(const h2_libco_config_t *config,
                                  h2_libco_t **out_core);

/**
 * Destroy an executor after every task has been joined.
 *
 * @param core Caller storage containing an owned executor; set to NULL on
 * success.
 * @return OK, BUSY while any task is unjoined, or an argument/context error.
 */
h2_libco_result_t h2_libco_destroy(h2_libco_t **core);

/**
 * Allocate and enqueue a task without running its entry inline.
 *
 * The executor retains @p entry and @p user until the task completes. A task
 * handle remains valid as a rejected tombstone after join and until executor
 * destruction.
 *
 * @param core Borrowed executor used by the current root or running task.
 * @param options Optional borrowed stack configuration.
 * @param entry Required task entry callback.
 * @param user Borrowed entry context retained through task completion.
 * @param out_task Caller storage containing NULL; receives a borrowed handle.
 * @return OK, INVALID_ARG, NO_MEMORY, or STACK_CORRUPT.
 */
h2_libco_result_t h2_libco_task_start(
    h2_libco_t *core,
    const h2_libco_task_options_t *options,
    h2_libco_task_entry_fn_t entry,
    void *user,
    h2_libco_task_t **out_task);

/**
 * Request cooperative cancellation.
 *
 * A waiting task becomes next-turn ready. A running task is never preempted and
 * observes cancellation when it next calls a suspension operation.
 *
 * @param core Borrowed owning executor.
 * @param task Borrowed unjoined task handle owned by @p core.
 * @return OK or an argument/state error.
 */
h2_libco_result_t h2_libco_task_cancel(h2_libco_t *core,
                                       h2_libco_task_t *task);

/**
 * Join a task and optionally return its entry result.
 *
 * A task caller waits cooperatively. A root caller receives BUSY unless the
 * target is already complete.
 *
 * @param core Borrowed owning executor.
 * @param task Borrowed completed or joinable task handle.
 * @param out_entry_result Optional caller storage for the entry return value.
 * @return OK, BUSY, CANCELLED, or an argument/state error.
 */
h2_libco_result_t h2_libco_task_join(h2_libco_t *core,
                                     h2_libco_task_t *task,
                                     int *out_entry_result);

/**
 * Yield the current task until a later scheduler turn.
 *
 * @param core Borrowed executor whose task is currently running.
 * @return OK after resumption, CANCELLED, or INVALID_STATE.
 */
h2_libco_result_t h2_libco_yield(h2_libco_t *core);

/**
 * Resume at most the start-of-turn snapshot and @p work_budget tasks.
 *
 * Each task is considered at most once. Tasks made ready during this call are
 * deferred to a later call. This operation is valid only on the bound root.
 *
 * @param core Borrowed executor bound to the current root.
 * @param work_budget Maximum number of ready tasks to process.
 * @param out_resumed Optional caller storage, cleared before validation.
 * @return OK, STACK_CORRUPT, or INVALID_STATE.
 */
h2_libco_result_t h2_libco_schedule(h2_libco_t *core,
                                    size_t work_budget,
                                    size_t *out_resumed);

/**
 * Wait in the current task for an edge-triggered key, timeout, or cancel.
 *
 * Callers must maintain and recheck their own completion predicate because a
 * wake made before this call is not retained.
 *
 * @param core Borrowed executor whose task is currently running.
 * @param wait_key Nonzero caller-owned opaque key.
 * @param timeout_ms Finite milliseconds or H2_LIBCO_WAIT_FOREVER.
 * @return WOKEN, TIMEOUT, CANCELLED, or INVALID_ARG.
 */
h2_libco_result_t h2_libco_wait(h2_libco_t *core,
                                uintptr_t wait_key,
                                uint32_t timeout_ms);

/**
 * Make matching waiters ready without switching context.
 *
 * Matching tasks are selected in FIFO wait order. Zero is a no-op and
 * H2_LIBCO_WAKE_ALL selects all current waiters.
 *
 * @param core Borrowed executor used by the current root or running task.
 * @param wait_key Nonzero caller-owned opaque key.
 * @param max_waiters Maximum number of waiters to make ready.
 * @param out_woken Optional caller storage, cleared before validation.
 * @return OK or INVALID_ARG.
 */
h2_libco_result_t h2_libco_wake(h2_libco_t *core,
                                uintptr_t wait_key,
                                size_t max_waiters,
                                size_t *out_woken);

/** Return the executor-owned PAL Task provider, or NULL for an invalid core. */
const h2_pal_task_api_t *h2_libco_task_api(h2_libco_t *core);

/** Cancel a task created through this executor's PAL Task API. */
h2_pal_result_t h2_libco_pal_task_cancel(h2_libco_t *core,
                                         h2_pal_task_t *task);
/** Return the executor-owned PAL Time provider, or NULL for an invalid core. */
const h2_pal_time_api_t *h2_libco_time_api(h2_libco_t *core);
/** Return the executor-owned PAL Queue provider, or NULL for an invalid core. */
const h2_pal_queue_api_t *h2_libco_queue_api(h2_libco_t *core);
/** Return the executor-owned PAL Sync provider, or NULL for an invalid core. */
const h2_pal_sync_api_t *h2_libco_sync_api(h2_libco_t *core);

#ifdef __cplusplus
}
#endif

#endif
