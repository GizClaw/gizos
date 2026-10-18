#ifndef H2_ESP_PLATFORM_SAFE_CALL_H
#define H2_ESP_PLATFORM_SAFE_CALL_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*h2_esp_platform_safe_call_cb_t)(void *context);

/**
 * Run a cache-sensitive ESP backend callback synchronously.
 *
 * A caller already using an internal stack and internal context runs directly
 * when the callback code is cache-safe. Other callers synchronously reuse one
 * fixed internal worker stack and a bounded internal context buffer. No
 * per-call task, stack, task control block, or context allocation is performed.
 */
h2_pal_result_t h2_esp_platform_safe_call(
    h2_esp_platform_safe_call_cb_t callback,
    void *context,
    size_t context_size,
    size_t stack_depth);

/**
 * @brief Run synchronously on the existing 16 KiB Internal worker, including
 * for an Internal-stack caller.
 * @param callback Native callback, invoked once on successful dispatch.
 * @param context Borrowed writable storage, retained only until return.
 * @param context_size Bytes copied to the worker and back, from 1 through 1024.
 * @param stack_depth Validated budget from 1024 through 16384 bytes; this does
 * not resize the worker or measure the caller's remaining stack.
 * @return OK after callback/copy-back, INVALID_ARG for invalid input or ISR
 * context, TASK on initialization/dispatch failure, or INVALID_STATE for an
 * unsafe nested invocation. Callback results remain in the caller's context.
 *
 * Call only from task context. A nested call already on this worker executes
 * directly only with an Internal stack/context and cache-safe callback code
 * (IRAM, or enabled PSRAM XIP); otherwise it returns INVALID_STATE without
 * queueing to itself or changing the active context. Initialization/dispatch
 * failure returns the actual error, without falling back to the caller stack.
 *
 * The caller retains its outer locks while waiting. The callback must perform
 * only the prepared native operation and must not acquire a caller-held lock
 * or a higher-level lock preceding this dispatch in that owner's lock order.
 */
h2_pal_result_t h2_esp_platform_safe_call_on_worker(
    h2_esp_platform_safe_call_cb_t callback,
    void *context,
    size_t context_size,
    size_t stack_depth);

/**
 * Acquire the single fixed Internal RAM scratch buffer used by cache-sensitive
 * filesystem, NVS, and partition I/O.
 *
 * The lease is synchronous and blocks until the shared buffer is available.
 * Callers must release it after all safe calls and caller-side copies using the
 * returned storage have completed.
 */
h2_pal_result_t h2_esp_platform_safe_io_acquire(
    uint8_t **out_buffer,
    size_t *out_capacity);

/**
 * Release the fixed safe-I/O scratch buffer.
 */
void h2_esp_platform_safe_io_release(void);

#ifdef __cplusplus
}
#endif

#endif
