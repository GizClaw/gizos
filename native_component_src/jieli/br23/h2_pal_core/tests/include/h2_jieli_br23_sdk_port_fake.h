/**
 * @file h2_jieli_br23_sdk_port_fake.h
 * @brief Deterministic single-threaded fake of the br23 SDK port for host tests.
 */

#ifndef H2_JIELI_BR23_SDK_PORT_FAKE_H
#define H2_JIELI_BR23_SDK_PORT_FAKE_H

#include "h2_jieli_br23_sdk_port.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_JIELI_FAKE_LOG_CAPACITY 4096u
#define H2_JIELI_FAKE_TIMER_CAPACITY 8u

/** Resets clocks, captured output, counters and timers. */
void h2_jieli_fake_reset(void);

/** Captured debug UART bytes (NUL-terminated) and their length. */
const char *h2_jieli_fake_log_output(void);
size_t h2_jieli_fake_log_length(void);

/** Advances the fake millisecond clock; sleeps also advance it. */
void h2_jieli_fake_advance_ms(uint32_t ms);
void h2_jieli_fake_set_time_ms(uint32_t ms);
uint32_t h2_jieli_fake_sleep_total_ms(void);

/** Outstanding allocations (alloc minus free) for leak checks. */
int h2_jieli_fake_live_allocations(void);

/** Fires every registered timer whose deadline has passed at the fake clock. */
void h2_jieli_fake_run_timers(void);
size_t h2_jieli_fake_timer_count(void);
/**
 * Hook run after a timer has been taken off the fake list (dispatched) but
 * before its callback enters, once per run_timers pass; models a destroy
 * racing a dispatched callback. Pass NULL to clear.
 */
void h2_jieli_fake_set_timer_dispatch_hook(void (*hook)(void));

/** Handle reported by h2_jieli_sdk_task_current(); defaults to a fixed one. */
void h2_jieli_fake_set_current_task(const void *handle);

/** Records the last task creation request. */
int h2_jieli_fake_task_create_calls(void);
const char *h2_jieli_fake_last_task_name(void);
size_t h2_jieli_fake_last_task_stack_bytes(void);
/** Runs the last created task entry synchronously (for trampoline tests). */
void h2_jieli_fake_run_last_task_once(void);
/** When non-zero, task creation fails. */
void h2_jieli_fake_fail_task_create(int fail);

#ifdef __cplusplus
}
#endif

#endif /* H2_JIELI_BR23_SDK_PORT_FAKE_H */
