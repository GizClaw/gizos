#ifndef H2_ESP_PLATFORM_ARENA_H
#define H2_ESP_PLATFORM_ARENA_H

#include "h2_mem_arena.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_platform_arena h2_esp_platform_arena_t;

/** Optional census callsite probe. Xtensa walks the IDF task stack to the
 * application allocation frame; other ESP architectures return UNSUPPORTED.
 * Failure only loses site evidence and never affects an allocation. */
h2_pal_result_t h2_esp_platform_arena_capture_site(
    void *user, uintptr_t *out_caller, uintptr_t *out_outer);

typedef struct h2_esp_platform_arena_config {
    size_t reserved_bytes;
    size_t small_request_max;
    size_t small_pool_bytes;
    /** Optional borrowed diagnostic name; must outlive the arena. */
    const char *name;
    /** Opt in to the shared system PSRAM heap on exhaustion of both pools.
     * Defaults to false: allocation failure returns NULL to the consumer. */
    bool spill_to_system;
} h2_esp_platform_arena_config_t;

/** Core pool statistics, sampled under the ESP mutex. */
typedef h2_mem_arena_stats_t h2_esp_platform_arena_stats_t;

/**
 * Reserve one PSRAM block and wrap the portable size-class arena with a
 * priority-inheriting FreeRTOS mutex. Task context only. Returns INVALID_ARG
 * for invalid size, NO_MEMORY on reservation/control/mutex failure; *out_arena
 * is NULL on error. Alloc/realloc/free and stats serialize on the mutex; never
 * call from an ISR. Exhaustion returns NULL unless spill_to_system is true.
 * Consumers with result codes propagate this as NO_MEMORY. Realloc can
 * migrate both ways; failure preserves the original allocation. Zero size frees
 * and returns NULL.
 */
h2_pal_result_t
h2_esp_platform_arena_create(const h2_esp_platform_arena_config_t *config,
                             h2_esp_platform_arena_t **out_arena);

/** Borrow the allocator until destroy. NULL arena returns NULL. Its user
 * pointer is the ESP arena, not the portable core. Failed nonzero alloc and
 * realloc requests emit rate-limited ESP-only refusal diagnostics. */
const h2_pal_mem_api_t *
h2_esp_platform_arena_mem(h2_esp_platform_arena_t *arena);

/** Borrow the portable core for block inspection until destroy. NULL arena
 * returns NULL; the allocator's user pointer is not the core. */
h2_mem_arena_t *h2_esp_platform_arena_core(h2_esp_platform_arena_t *arena);

/** Copy a mutex-protected snapshot; NULL arena returns INVALID_STATE and zeros.
 * Logging is caller-owned and must happen after this call returns. */
h2_pal_result_t
h2_esp_platform_arena_stats(h2_esp_platform_arena_t *arena,
                            h2_esp_platform_arena_stats_t *out_stats);

/** @brief Log live system spills grouped by captured caller frames.
 * Borrows arena and optional phase for this call; NULL arena is a no-op.
 * Task context only; serialize the snapshot under the arena mutex, then log
 * after unlocking. At most once per minute per arena unless force is true.
 * ESP tracks up to 512 live spill bases in an optional PSRAM table. Table
 * allocation failure or capacity overflow drops tracking, not allocations;
 * untracked counts cumulative dropped tracking events, not live blocks.
 * Bytes include arena headers/padding. Realloc keeps the original frames;
 * free or migration back into the arena removes the entry. Up to 24 groups
 * are printed; other counts tracked bytes outside those groups.
 * Xtensa captures up to six frames; other ESP architectures report zero PCs.
 * Host builds compile out the registry, backtrace and logging. Destroy must
 * wait for this call, just as it waits for other borrowers.
 */
void h2_esp_platform_arena_log_spills(h2_esp_platform_arena_t *arena,
                                      const char *phase, bool force);

/**
 * Destroy after all borrowers/tasks have stopped using the API. Refuses live
 * arena OR fallback allocations with INVALID_STATE and preserves the instance.
 * NULL is OK; a successfully destroyed non-NULL handle must not be reused.
 */
h2_pal_result_t h2_esp_platform_arena_destroy(h2_esp_platform_arena_t *arena);

#ifdef __cplusplus
}
#endif
#endif
