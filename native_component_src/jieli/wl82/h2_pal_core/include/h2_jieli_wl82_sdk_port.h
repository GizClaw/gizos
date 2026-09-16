/**
 * @file h2_jieli_wl82_sdk_port.h
 * @brief Minimal JieLi wl82 SDK surface consumed by the PAL core providers.
 *
 * The providers in this component only call these functions, so the native
 * firmware build implements them on top of the SDK (`h2_jieli_wl82_sdk_port.c`)
 * while host tests link a deterministic fake. Nothing in this header includes
 * SDK headers; handles are opaque.
 */

#ifndef H2_JIELI_WL82_SDK_PORT_H
#define H2_JIELI_WL82_SDK_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Block forever when passed as a timeout. */
#define H2_JIELI_SDK_WAIT_FOREVER UINT32_MAX

typedef struct h2_jieli_sdk_mutex h2_jieli_sdk_mutex_t;
typedef struct h2_jieli_sdk_sem h2_jieli_sdk_sem_t;

/** Single-attempt hardware byte lock for exception-safe capture. Never spins.
 * Storage must start at zero. Unlock publishes preceding writes. */
int h2_jieli_sdk_try_lock_byte(volatile uint8_t *lock);
void h2_jieli_sdk_unlock_byte(volatile uint8_t *lock);
void h2_jieli_sdk_capture_barrier(void);

/* ---- Memory (SDK heap) --------------------------------------------------- */

void *h2_jieli_sdk_malloc(size_t size);
void *h2_jieli_sdk_realloc(void *ptr, size_t size);
void h2_jieli_sdk_free(void *ptr);

/* ---- Layout-selected debug output ---------------------------------------- */

/** Writes length-delimited log text through the SDK buffered debug producer.
 * The text excludes embedded NUL bytes; no trailing NUL is required. The board
 * layout selects UART/USB delivery. This is not a binary transport and is
 * ISR-unsafe; delivery remains subject to the SDK debug configuration. */
void h2_jieli_sdk_debug_write(const char *data, size_t length);

/* ---- Time ---------------------------------------------------------------- */

/** Milliseconds since boot from the SDK tick timer; wraps at 32 bits. */
uint32_t h2_jieli_sdk_time_ms(void);

/** Reads the board-owned monotonic clock; returns a PAL result code. */
int h2_jieli_sdk_time_us(uint64_t *out_us);
/** Yields the calling task for at least `ms` milliseconds (SDK tick granularity). */
void h2_jieli_sdk_sleep_ms(uint32_t ms);
/** Returns the SDK OS tick period in milliseconds. */
uint32_t h2_jieli_sdk_tick_ms(void);

/* ---- Synchronization ----------------------------------------------------- */

h2_jieli_sdk_mutex_t *h2_jieli_sdk_mutex_create(void);
void h2_jieli_sdk_mutex_destroy(h2_jieli_sdk_mutex_t *mutex);
/** Returns 0 on success, 1 on timeout, negative on error. */
int h2_jieli_sdk_mutex_lock(h2_jieli_sdk_mutex_t *mutex, uint32_t timeout_ms);
int h2_jieli_sdk_mutex_unlock(h2_jieli_sdk_mutex_t *mutex);

h2_jieli_sdk_sem_t *h2_jieli_sdk_sem_create(uint32_t initial_count);
h2_jieli_sdk_sem_t *h2_jieli_sdk_sem_create_bounded(uint32_t initial_count, uint32_t max_count);
void h2_jieli_sdk_sem_destroy(h2_jieli_sdk_sem_t *sem);
/** Returns 0 on success, 1 on timeout, negative on error. */
int h2_jieli_sdk_sem_take(h2_jieli_sdk_sem_t *sem, uint32_t timeout_ms);
/** Returns 0 on success, 1 when full, negative on error. */
int h2_jieli_sdk_sem_give(h2_jieli_sdk_sem_t *sem);

/* ---- Tasks --------------------------------------------------------------- */

/**
 * Creates an SDK task running `entry(ctx)`.
 *
 * policy_name selects the generated target policy (NULL selects its default).
 * native_name must be unique for each live task. The stack is the greater of
 * the policy budget and stack_bytes rounded to SDK words. Returns 0 on success,
 * negative on error.
 */
int h2_jieli_sdk_task_create(void (*entry)(void *ctx), void *ctx, const char *policy_name, const char *native_name, size_t stack_bytes);
/** Deletes a task that has finished and parked, by its unique SDK name. */
int h2_jieli_sdk_task_delete(const char *name);
/**
 * Parks the calling SDK task forever once its PAL entry returned; SDK tasks
 * must never return from their entry function. The host fake returns instead.
 */
void h2_jieli_sdk_task_park(void);
/**
 * Handle of the calling SDK task (`xTaskGetCurrentTaskHandle()`). The handle
 * is the task's identity — unlike the task name, two tasks never share one —
 * and stays valid while the task lives. NULL before the scheduler runs.
 */
const void *h2_jieli_sdk_task_current(void);

/** Fixed envelope size, bounded by the SDK dispatcher read buffer. */
#define H2_JIELI_SDK_EVENT_MESSAGE_SIZE 32u
typedef void (*h2_jieli_sdk_event_handler_t)(const void *message, size_t size);
/** Start the persistent h2_sysevt owner task once; wait at most 1000 ms for
 * registration. Return 0 on readiness, negative on creation/registration/timeout.
 * Delivery is serial on that task inside os_taskq_pend; messages are borrowed
 * until the handler returns. Later starts do not create another task. */
int h2_jieli_sdk_event_dispatcher_start(h2_jieli_sdk_event_handler_t handler);
/** Copy exactly 32 bytes into sys_event (type 0x0100, from 0x50).
 * Return 0 on success, 1 on ring full (-12), negative on error/invalid input. */
int h2_jieli_sdk_event_post(const void *message, size_t size);
/** Nonzero in interrupt context; PAL posts must reject this context. */
int h2_jieli_sdk_in_interrupt(void);

/* ---- Timers -------------------------------------------------------------- */

/** Execute synchronously on the SDK timer service task, inline when already
 * there. The operation and context remain borrowed until this call returns.
 * Reject interrupt/pre-scheduler calls. Timer callbacks must not block. */
int h2_jieli_sdk_timer_call(int (*operation)(void *ctx), void *ctx);

/**
 * Registers an SDK timer calling `callback(ctx)` after `period_ms`
 * milliseconds: periodic (sys_timer_add) when `repeat` is non-zero, otherwise
 * one-shot (sys_timeout_add, which releases itself after firing). Returns a
 * non-zero id or 0 on error.
 *
 * Dispatch contract (SDK `timer.h`): the systimer thread only keeps time; the
 * callback runs on the task that registered the timer, serialised with that
 * task's other timer callbacks. Deleting a timer does not recall a fire that
 * is already queued to the task, so callers that need to release `ctx` must
 * defer the release behind a later timer on the same task. Callers therefore
 * have to register and cancel a timer from one and the same task. The PAL
 * facade uses timer_call() to make that task the SDK timer service rather
 * than imposing an SDK event loop on each application task.
 */
uint16_t h2_jieli_sdk_timer_add(void *ctx, void (*callback)(void *ctx), uint32_t period_ms, int repeat);
/** Cancels a timer registered with the same `repeat` flavour. */
void h2_jieli_sdk_timer_del(uint16_t id, int repeat);

#ifdef __cplusplus
}
#endif

#endif /* H2_JIELI_WL82_SDK_PORT_H */
