/**
 * @file h2_jieli_wl82_platform_core.h
 * @brief PAL core providers for the JieLi AC791N (wl82) SDK.
 *
 * Every accessor returns a process-wide API object backed by the SDK heap,
 * debug UART, tick timer, FreeRTOS-based os_api primitives and sys timers via
 * `h2_jieli_wl82_sdk_port.h`. Providers are stateless between boots and do not
 * own board wiring; the launcher re-initialises the debug UART for its board
 * before using the Log provider.
 */

#ifndef H2_JIELI_WL82_PLATFORM_CORE_H
#define H2_JIELI_WL82_PLATFORM_CORE_H

#include "h2/pal/os/h2_pal_firmware_info.h"
#include "h2/pal/os/h2_pal_log.h"
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

/** Longest formatted log line, including level/scope prefix and CRLF. */
#define H2_JIELI_WL82_LOG_LINE_MAX 320u

/** SDK heap allocator (malloc/realloc/free). */
const h2_pal_mem_api_t *h2_jieli_wl82_platform_mem_api(void);

/** Debug UART text sink: `[LEVEL][scope] message\r\n`. */
const h2_pal_log_api_t *h2_jieli_wl82_platform_log_api(void);

/**
 * Monotonic time from the SDK tick timer (32-bit wrap extended to 64 bits
 * while queried at least once per wrap), sleep via the OS delay. Wall time is
 * not supported on this target.
 */
const h2_pal_time_api_t *h2_jieli_wl82_platform_time_api(void);

/** Mutex, counting semaphore and condition variables on os_api. */
const h2_pal_sync_api_t *h2_jieli_wl82_platform_sync_api(void);

/** Internal wl82 helper for compound providers. Same wait contract as Sync,
 * with explicit mutex ownership on error (SDK relock may fail). The caller
 * enters holding mutex; out_locked is set on every path. */
h2_pal_result_t h2_jieli_wl82_cond_wait_owned(
    h2_pal_cond_t *cond, h2_pal_mutex_t *mutex, uint32_t timeout_ms, int *out_locked);

/** Thread-safe in-process event fanout used by BLE, network and loader services. */
const h2_pal_system_event_api_t *h2_jieli_wl82_platform_system_event_api(void);

/** Bounded FIFO with one mutex-protected ring and predicate-based condition waits. */
const h2_pal_queue_api_t *h2_jieli_wl82_platform_queue_api(void);

/** Task start on os_task_create with PAL completion and join semantics. */
const h2_pal_task_api_t *h2_jieli_wl82_platform_task_api(void);

/** One-shot and periodic timers on the SDK sys timer service. */
const h2_pal_timer_api_t *h2_jieli_wl82_platform_timer_api(void);

/** Reports the firmware version baked in at build time (H2_JIELI_FIRMWARE_VERSION). */
const h2_pal_firmware_info_api_t *h2_jieli_wl82_platform_firmware_info_api(void);

#ifdef __cplusplus
}
#endif

#endif /* H2_JIELI_WL82_PLATFORM_CORE_H */
