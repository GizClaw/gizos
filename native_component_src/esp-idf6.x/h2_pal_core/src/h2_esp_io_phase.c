#include "h2_esp_io_phase.h"

#if H2_ESP_IO_PHASE_DIAGNOSTICS
#include "esp_timer.h"

#include <stdio.h>

uint64_t h2_esp_io_phase_now(void) {
    const int64_t now = esp_timer_get_time();
    return now >= 0 ? (uint64_t)now : 0u;
}

uint64_t h2_esp_io_phase_elapsed(uint64_t started, uint64_t ended) {
    return ended >= started ? ended - started : 0u;
}

static uint64_t add_saturated(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

void h2_esp_io_phase_add(h2_esp_io_phase_t *total,
                          const h2_esp_io_phase_t *call) {
    total->fs_wait_us = add_saturated(total->fs_wait_us, call->fs_wait_us);
    total->scratch_wait_us =
        add_saturated(total->scratch_wait_us, call->scratch_wait_us);
    total->shared_wait_us =
        add_saturated(total->shared_wait_us, call->shared_wait_us);
    total->dispatch_us = add_saturated(total->dispatch_us, call->dispatch_us);
    total->native_us = add_saturated(total->native_us, call->native_us);
    if (call->native_max_us > total->native_max_us)
        total->native_max_us = call->native_max_us;
    total->wake_copy_us =
        add_saturated(total->wake_copy_us, call->wake_copy_us);
    total->calls = add_saturated(total->calls, call->calls);
    total->direct_calls =
        add_saturated(total->direct_calls, call->direct_calls);
}

void h2_esp_io_phase_report(const char *owner, unsigned op, size_t bytes,
                             int result, const h2_esp_io_phase_t *phase) {
    const uint64_t ended = h2_esp_io_phase_now();
    const uint64_t wall =
        h2_esp_io_phase_elapsed(phase->started_us, ended);
    if (wall < 100000u) return;
    printf("H2_ESP_IO_PHASE owner=%s op=%u bytes=%zu rc=%d "
           "started_us=%llu ended_us=%llu wall_us=%llu fs_wait_us=%llu "
           "scratch_wait_us=%llu shared_wait_us=%llu dispatch_us=%llu "
           "native_us=%llu native_max_us=%llu wake_copy_us=%llu calls=%llu "
           "direct_calls=%llu\n",
           owner, op, bytes, result,
           (unsigned long long)phase->started_us, (unsigned long long)ended,
           (unsigned long long)wall,
           (unsigned long long)phase->fs_wait_us,
           (unsigned long long)phase->scratch_wait_us,
           (unsigned long long)phase->shared_wait_us,
           (unsigned long long)phase->dispatch_us,
           (unsigned long long)phase->native_us,
           (unsigned long long)phase->native_max_us,
           (unsigned long long)phase->wake_copy_us,
           (unsigned long long)phase->calls,
           (unsigned long long)phase->direct_calls);
}
#endif
