/**
 * @file h2_jieli_wl82_platform_core.h
 * @brief PAL core providers for the JieLi AC791N (wl82) SDK.
 *
 * Every accessor returns a process-wide API object backed by the SDK heap,
 * buffered debug producer, board-owned monotonic clock, os_api primitives and
 * sys timers via `h2_jieli_wl82_sdk_port.h`. Resource and service lifecycles
 * follow their PAL contracts. Providers do not own board wiring; the shared
 * board layout configures the clock and UART/USB diagnostic delivery.
 */

#ifndef H2_JIELI_WL82_PLATFORM_CORE_H
#define H2_JIELI_WL82_PLATFORM_CORE_H

#include "h2/pal/os/h2_pal_firmware_info.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_atomic.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/os/h2_pal_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

const h2_pal_atomic_api_t *h2_jieli_wl82_platform_atomic_api(void);

/** Longest formatted log line, including level/scope prefix and CRLF. */
#define H2_JIELI_WL82_LOG_LINE_MAX 320u

/** SDK heap allocator (malloc/realloc/free). */
const h2_pal_mem_api_t *h2_jieli_wl82_platform_mem_api(void);

/** Buffered text sink: `[LEVEL][scope] message\r\n`.
 * The board layout selects UART/USB delivery; calls are not ISR-safe. */
const h2_pal_log_api_t *h2_jieli_wl82_platform_log_api(void);

/**
 * Milliseconds and microseconds share the board-owned 64-bit monotonic clock
 * exposed by h2_jieli_sdk_time_us(). Missing board clock support is reported
 * as UNSUPPORTED, not substituted with OS tick timestamps. Sleep uses the OS
 * delay; wall time is unsupported and its status is invalid/unknown.
 */
const h2_pal_time_api_t *h2_jieli_wl82_platform_time_api(void);

/** Mutex, counting semaphore and condition variables on os_api. */
const h2_pal_sync_api_t *h2_jieli_wl82_platform_sync_api(void);

/** Internal wl82 helper for compound providers. Same wait contract as Sync,
 * with explicit mutex ownership on error (SDK relock may fail). The caller
 * enters holding mutex; out_locked is set on every path. */
h2_pal_result_t h2_jieli_wl82_cond_wait_owned(
    h2_pal_cond_t *cond, h2_pal_mutex_t *mutex, uint32_t timeout_ms, int *out_locked);

/** Asynchronous SDK sys_event fanout on the persistent h2_sysevt task.
 * Post copies a 32-byte envelope into the SDK ring (type 0x0100, from 0x50).
 * Payloads up to 8 bytes are inline; 9..1024 bytes use an owned heap copy freed
 * after delivery. Larger payloads return INVALID_ARG, allocation failure
 * returns NO_MEMORY, and interrupt-context posts return INVALID_STATE.
 * No caller memory is borrowed after post; callback payloads last only for
 * that callback. Post reports enqueue status, not handler return values.
 * Four in-flight events bound PAL depth. Depth exhaustion or SDK ring full
 * returns FULL and increments the image-lifetime overflow counter. Other SDK
 * post errors return IO. timeout_ms bounds only registry lock acquisition.
 * Unmatched events return OK without entering the SDK ring.
 * Each successful init acquires one owner (maximum 16383; overflow is FULL).
 * Pair each with deinit, after retiring that owner's subscriptions. The last
 * deinit closes admission; queued operation references defer destruction and
 * preserve delivery to still-live subscriptions, even after handler deinit.
 * Init during INITIALIZING/CLOSING returns BUSY; inactive post/subscribe return
 * INVALID_STATE. Dispatcher start failure returns TASK with no owner acquired.
 * Extra inactive deinit is harmless; never release another owner's reference.
 * External unsubscribe waits for queued and running callbacks, including in
 * CLOSING, then handler_user can be freed. Any dispatcher-task unsubscribe,
 * of itself or another subscription, stops admission without waiting. Keep
 * handler_user alive for callbacks already admitted; retiring queued events
 * are consumed without a callback and slots are reused only after quiescence.
 * The fixed registry has H2_PAL_SYSTEM_EVENT_TYPE_COUNT + 8 slots. Post takes
 * a registry-locked 64-bit generation ceiling, excluding subscriptions added
 * before dispatch but after enqueue admission. Generations never wrap: at
 * UINT64_MAX subscribe returns FULL. Only complete teardown and fresh init
 * reset the epoch. The dispatcher task and SDK registration persist forever.
 * Handlers run serially in SDK enqueue order and must stay short/nonblocking:
 * blocking the callback chain for 40 seconds causes SDK assertion or reset. */
const h2_pal_system_event_api_t *h2_jieli_wl82_platform_system_event_api(void);

/** Image-lifetime count of posts rejected by PAL depth or SDK ring capacity. */
uint32_t h2_jieli_wl82_platform_system_event_overflow_count(void);

/** Bounded FIFO with one mutex-protected ring and predicate-based condition waits. */
const h2_pal_queue_api_t *h2_jieli_wl82_platform_queue_api(void);

/** Task start on os_task_create with PAL completion and join semantics. */
const h2_pal_task_api_t *h2_jieli_wl82_platform_task_api(void);

/** One-shot and periodic timers on the SDK sys timer service.
 * Lifecycle operations synchronously dispatch to the SDK sys_timer task;
 * calls from its callbacks execute inline. Not ISR/pre-scheduler safe.
 * Callbacks must stay short and nonblocking. Stop invalidates queued fires;
 * reset uses a fresh registration context. Old contexts and destroyed timers
 * are reclaimed later on the sys_timer task. Reclaim slot exhaustion returns
 * UNAVAILABLE without releasing caller ownership; allocation/registration
 * failure returns NO_MEMORY. A resource failure while rearming leaves the
 * timer stopped and retryable. A successful destroy consumes the handle
 * immediately. */
const h2_pal_timer_api_t *h2_jieli_wl82_platform_timer_api(void);

/** Reports the firmware version baked in at build time (H2_JIELI_FIRMWARE_VERSION). */
const h2_pal_firmware_info_api_t *h2_jieli_wl82_platform_firmware_info_api(void);

#ifdef __cplusplus
}
#endif

#endif /* H2_JIELI_WL82_PLATFORM_CORE_H */
