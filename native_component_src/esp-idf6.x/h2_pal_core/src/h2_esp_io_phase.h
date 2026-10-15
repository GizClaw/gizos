#ifndef H2_ESP_IO_PHASE_H
#define H2_ESP_IO_PHASE_H

#ifndef H2_ESP_IO_PHASE_DIAGNOSTICS
#define H2_ESP_IO_PHASE_DIAGNOSTICS 0
#endif

#if H2_ESP_IO_PHASE_DIAGNOSTICS
#include "h2_esp_platform_safe_call.h"

#include <stdint.h>

/* Caller-owned diagnostic scalars. The worker publishes its timestamps using
 * the existing request/done synchronization; there is no second reader. */
typedef struct h2_esp_io_phase {
    uint64_t started_us;
    uint64_t fs_wait_us;
    uint64_t scratch_wait_us;
    uint64_t shared_wait_us;
    uint64_t dispatch_us;
    uint64_t native_us;
    uint64_t native_max_us;
    uint64_t wake_copy_us;
    uint64_t calls;
    uint64_t direct_calls;
} h2_esp_io_phase_t;

uint64_t h2_esp_io_phase_now(void);
uint64_t h2_esp_io_phase_elapsed(uint64_t started, uint64_t ended);
void h2_esp_io_phase_add(h2_esp_io_phase_t *total,
                          const h2_esp_io_phase_t *call);
/* Only after every enclosing FS/scratch mutex has been released. No paths,
 * preference namespaces/keys, values or buffer contents are reported. */
void h2_esp_io_phase_report(const char *owner, unsigned op, size_t bytes,
                             int result, const h2_esp_io_phase_t *phase);
h2_pal_result_t h2_esp_platform_safe_call_timed(
    h2_esp_platform_safe_call_cb_t callback, void *context,
    size_t context_size, size_t stack_depth, h2_esp_io_phase_t *phase);
#endif

#endif
