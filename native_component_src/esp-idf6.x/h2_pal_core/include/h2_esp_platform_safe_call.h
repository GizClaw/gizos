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
