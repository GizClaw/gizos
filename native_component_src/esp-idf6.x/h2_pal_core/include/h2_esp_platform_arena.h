#ifndef H2_ESP_PLATFORM_ARENA_H
#define H2_ESP_PLATFORM_ARENA_H

#include "h2_mem_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_platform_arena h2_esp_platform_arena_t;

typedef struct h2_esp_platform_arena_config {
    size_t reserved_bytes;
    size_t small_request_max;
    size_t small_pool_bytes;
    /** Optional borrowed diagnostic name; must outlive the arena. */
    const char *name;
} h2_esp_platform_arena_config_t;

/** Core pool statistics, sampled under the ESP mutex. */
typedef h2_mem_arena_stats_t h2_esp_platform_arena_stats_t;

/**
 * Reserve one PSRAM block and wrap the portable size-class arena with a
 * priority-inheriting FreeRTOS mutex. Task context only. Returns INVALID_ARG
 * for invalid size, NO_MEMORY on reservation/control/mutex failure; *out_arena
 * is NULL on error. Alloc/realloc/free and stats serialize on the mutex; never
 * call from an ISR. Exhaustion falls back to the PSRAM heap. Realloc can
 * migrate both ways; failure preserves the original allocation. Zero size frees
 * and returns NULL.
 */
h2_pal_result_t
h2_esp_platform_arena_create(const h2_esp_platform_arena_config_t *config,
                             h2_esp_platform_arena_t **out_arena);

/** Borrow the allocator until destroy. NULL arena returns NULL. */
const h2_pal_mem_api_t *
h2_esp_platform_arena_mem(h2_esp_platform_arena_t *arena);

/** Copy a mutex-protected snapshot; NULL arena returns INVALID_STATE and zeros.
 * Logging is caller-owned and must happen after this call returns. */
h2_pal_result_t
h2_esp_platform_arena_stats(h2_esp_platform_arena_t *arena,
                            h2_esp_platform_arena_stats_t *out_stats);

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
