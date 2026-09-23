#include "h2_esp_platform_arena.h"
#include "esp_heap_caps.h"
#if defined(ESP_PLATFORM)
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_log.h"
#include "esp_timer.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

struct h2_esp_platform_arena {
    h2_pal_mem_api_t watched;
    void *block;
    h2_mem_arena_t *core;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
};

static void arena_lock(void *user) {
    h2_esp_platform_arena_t *arena = user;
    if (xSemaphoreTake(arena->lock, portMAX_DELAY) != pdTRUE)
        abort();
}

static void arena_unlock(void *user) {
    h2_esp_platform_arena_t *arena = user;
    if (xSemaphoreGive(arena->lock) != pdTRUE)
        abort();
}

/* An arena request fails only when its pool is full and this spill to the
 * system PSRAM heap fails too; name that moment, rate-limited. */
static void log_spill_failure(const char *op, size_t bytes) {
    static unsigned failures;
    const unsigned count = ++failures;
    if (count > 16u && count % 64u != 0u)
        return;
#if defined(ESP_PLATFORM)
    ESP_LOGW("h2_arena",
             "H2_ARENA_SPILL_FAILED op=%s bytes=%u count=%u psram_free=%u "
             "psram_largest=%u internal_free=%u internal_largest=%u",
             op, (unsigned)bytes, count,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#else
    (void)op;
    (void)bytes;
#endif
}

#if defined(ESP_PLATFORM)
/* Live spills, keyed by pointer with the caller frames that made them. Every
 * spill path runs under the arena lock, which also guards this table. */
#define SPILL_SLOTS 512u
#define SPILL_FRAMES 6u
/* psram_alloc, arena_realloc and the h2_pal_mem entry point come first. */
#define SPILL_SKIP 3u
typedef struct spill_entry {
    void *ptr;
    uint32_t bytes;
    uint32_t pc[SPILL_FRAMES];
} spill_entry_t;
/* Kept in PSRAM: internal RAM is scarce and this table only watches spills. */
static spill_entry_t *s_spills;
static unsigned s_spill_untracked;

static void spill_capture(uint32_t pc[SPILL_FRAMES]) {
    memset(pc, 0, SPILL_FRAMES * sizeof(pc[0]));
#if defined(ESP_PLATFORM)
    esp_backtrace_frame_t frame = {0};
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    for (unsigned depth = 0u; depth < SPILL_SKIP + SPILL_FRAMES; ++depth) {
        if (depth >= SPILL_SKIP)
            pc[depth - SPILL_SKIP] = esp_cpu_process_stack_pc(frame.pc);
        if (frame.next_pc == 0u || !esp_backtrace_get_next_frame(&frame))
            break;
    }
#endif
}

static void spill_track(void *old_ptr, void *ptr, size_t bytes) {
    spill_entry_t *slot = NULL;
    if (s_spills == NULL) {
        static bool tried;
        if (tried) {
            ++s_spill_untracked;
            return;
        }
        tried = true;
        /* Allocated on the first spill, and from PSRAM: internal RAM is
         * scarce and this table only watches a path that should stay empty. */
        s_spills = heap_caps_malloc(SPILL_SLOTS * sizeof(*s_spills),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_spills == NULL) {
            ++s_spill_untracked;
            return;
        }
        memset(s_spills, 0, SPILL_SLOTS * sizeof(*s_spills));
    }
    for (unsigned i = 0u; i < SPILL_SLOTS; ++i) {
        if (old_ptr != NULL && s_spills[i].ptr == old_ptr) {
            slot = &s_spills[i];
            break;
        }
        if (slot == NULL && s_spills[i].ptr == NULL)
            slot = &s_spills[i];
    }
    if (slot == NULL) {
        ++s_spill_untracked;
        return;
    }
    const bool fresh = slot->ptr == NULL || slot->ptr != old_ptr;
    slot->ptr = ptr;
    slot->bytes = (uint32_t)bytes;
    if (fresh)
        spill_capture(slot->pc);
}

static void spill_untrack(void *ptr) {
    if (s_spills == NULL)
        return;
    for (unsigned i = 0u; i < SPILL_SLOTS; ++i) {
        if (s_spills[i].ptr == ptr) {
            s_spills[i].ptr = NULL;
            return;
        }
    }
}

#else
#define spill_track(old_ptr, ptr, bytes) ((void)0)
#define spill_untrack(ptr) ((void)0)
#endif

static void *psram_alloc(void *user, size_t bytes) {
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (user != NULL) {
        if (ptr == NULL)
            log_spill_failure("alloc", bytes);
        else
            spill_track(NULL, ptr, bytes);
    }
    return ptr;
}

static void *psram_realloc(void *user, void *ptr, size_t bytes) {
    void *moved = heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (user != NULL) {
        if (moved == NULL && bytes != 0u)
            log_spill_failure("realloc", bytes);
        else if (moved != NULL)
            spill_track(ptr, moved, bytes);
    }
    return moved;
}

static void psram_free(void *user, void *ptr) {
    if (user != NULL && ptr != NULL)
        spill_untrack(ptr);
    heap_caps_free(ptr);
}

static const h2_pal_mem_vtable_t psram_vtable = {
    .alloc = psram_alloc, .realloc = psram_realloc, .free = psram_free,
};
/* Non-NULL user marks arena spills; the reservation itself passes NULL. */
static char s_spill_user;
static const h2_pal_mem_api_t psram_mem = {.user = &s_spill_user,
                                           .vtable = &psram_vtable};

/* Wraps the core allocator so a refused request names itself: without a
 * fallback the failure is otherwise invisible until a caller reports it. */
static void log_refusal(h2_esp_platform_arena_t *arena, const char *op,
                        size_t bytes) {
#if defined(ESP_PLATFORM)
    static unsigned refusals;
    const unsigned count = ++refusals;
    if (count > 24u && count % 64u != 0u)
        return;
    h2_mem_arena_stats_t stats;
    if (h2_mem_arena_stats(arena->core, &stats) != H2_PAL_OK)
        return;
    h2_mem_arena_pool_inspection_t pools[2] = {0};
    (void)h2_mem_arena_inspect(arena->core, pools);
    ESP_LOGW("h2_arena",
             "H2_ARENA_REFUSED op=%s bytes=%u count=%u "
             "small_live=%u small_free=%u small_largest=%u "
             "large_live=%u large_free=%u large_largest=%u "
             "psram_free=%u psram_largest=%u internal_free=%u",
             op, (unsigned)bytes, count, (unsigned)stats.small.live_bytes,
             (unsigned)pools[0].free_bytes, (unsigned)pools[0].largest_free_block,
             (unsigned)stats.large.live_bytes, (unsigned)pools[1].free_bytes,
             (unsigned)pools[1].largest_free_block,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#else
    (void)arena;
    (void)op;
    (void)bytes;
#endif
}

static void *watched_alloc(void *user, size_t bytes) {
    h2_esp_platform_arena_t *arena = user;
    void *ptr = h2_pal_mem_alloc(h2_mem_arena_mem(arena->core), bytes);
    if (ptr == NULL && bytes != 0u)
        log_refusal(arena, "alloc", bytes);
    return ptr;
}

static void *watched_realloc(void *user, void *ptr, size_t bytes) {
    h2_esp_platform_arena_t *arena = user;
    void *moved = h2_pal_mem_realloc(h2_mem_arena_mem(arena->core), ptr, bytes);
    if (moved == NULL && bytes != 0u)
        log_refusal(arena, "realloc", bytes);
    return moved;
}

static void watched_free(void *user, void *ptr) {
    h2_esp_platform_arena_t *arena = user;
    h2_pal_mem_free(h2_mem_arena_mem(arena->core), ptr);
}

static const h2_pal_mem_vtable_t watched_vtable = {
    .alloc = watched_alloc, .realloc = watched_realloc, .free = watched_free,
};

h2_pal_result_t
h2_esp_platform_arena_create(const h2_esp_platform_arena_config_t *config,
                             h2_esp_platform_arena_t **out_arena) {
    if (out_arena == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_arena = NULL;
    if (config == NULL || config->reserved_bytes == 0u ||
        config->reserved_bytes > (size_t)PTRDIFF_MAX ||
        config->small_pool_bytes >= config->reserved_bytes)
        return H2_PAL_ERR_INVALID_ARG;
    /* Static mutex storage must remain internal, even with PSRAM XIP. */
    h2_esp_platform_arena_t *arena =
        heap_caps_malloc(sizeof(*arena), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (arena == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    memset(arena, 0, sizeof(*arena));
    h2_pal_result_t rc = H2_PAL_ERR_NO_MEMORY;
    arena->block = psram_mem.vtable->alloc(NULL, config->reserved_bytes);
    if (arena->block == NULL)
        goto unavailable;
    arena->lock = xSemaphoreCreateMutexStatic(&arena->lock_storage);
    if (arena->lock == NULL)
        goto unavailable;
    const h2_mem_arena_config_t core_config = {
        .name = config->name,
        .block = arena->block,
        .block_bytes = config->reserved_bytes,
        .small_request_max = config->small_request_max,
        .small_pool_bytes = config->small_pool_bytes,
        .fallback = config->spill_to_system ? &psram_mem : NULL,
        .lock = arena_lock,
        .unlock = arena_unlock,
        .lock_user = arena,
    };
    rc = h2_mem_arena_create(&core_config, &arena->core);
    if (rc != H2_PAL_OK)
        goto unavailable;
    arena->watched.user = arena;
    arena->watched.vtable = &watched_vtable;
    *out_arena = arena;
    return H2_PAL_OK;
unavailable:
    if (arena->lock != NULL)
        vSemaphoreDelete(arena->lock);
    heap_caps_free(arena->block);
    heap_caps_free(arena);
    return rc;
}

const h2_pal_mem_api_t *
h2_esp_platform_arena_mem(h2_esp_platform_arena_t *arena) {
    if (arena == NULL)
        return h2_mem_arena_mem(NULL);
    return &arena->watched;
}

h2_mem_arena_t *h2_esp_platform_arena_core(h2_esp_platform_arena_t *arena) {
    return arena != NULL ? arena->core : NULL;
}

h2_pal_result_t
h2_esp_platform_arena_stats(h2_esp_platform_arena_t *arena,
                            h2_esp_platform_arena_stats_t *out_stats) {
    return h2_mem_arena_stats(arena != NULL ? arena->core : NULL, out_stats);
}

h2_pal_result_t h2_esp_platform_arena_inspect(
    h2_esp_platform_arena_t *arena,
    h2_mem_arena_pool_inspection_t out_pools[2]) {
    return h2_mem_arena_inspect(arena != NULL ? arena->core : NULL, out_pools);
}

void h2_esp_platform_arena_log_spills(h2_esp_platform_arena_t *arena,
                                      const char *phase, bool force) {
#if defined(ESP_PLATFORM)
    static int64_t last_us;
    if (arena == NULL)
        return;
    const int64_t now_us = esp_timer_get_time();
    if (!force && last_us != 0 && now_us - last_us < 60 * 1000 * 1000)
        return;
    last_us = now_us;
    /* Group by the first two caller frames; the fixed table keeps this off
     * the heap it is diagnosing. */
    enum { GROUPS = 24 };
    struct {
        uint32_t pc[SPILL_FRAMES];
        uint32_t bytes;
        uint32_t blocks;
    } groups[GROUPS];
    unsigned used = 0u;
    uint32_t total = 0u, blocks = 0u, other = 0u;
    arena_lock(arena);
    for (unsigned i = 0u; s_spills != NULL && i < SPILL_SLOTS; ++i) {
        const spill_entry_t *entry = &s_spills[i];
        if (entry->ptr == NULL)
            continue;
        total += entry->bytes;
        ++blocks;
        unsigned g = 0u;
        while (g < used && (groups[g].pc[0] != entry->pc[0] ||
                            groups[g].pc[1] != entry->pc[1]))
            ++g;
        if (g == used) {
            if (used == GROUPS) {
                other += entry->bytes;
                continue;
            }
            memcpy(groups[g].pc, entry->pc, sizeof(groups[g].pc));
            groups[g].bytes = 0u;
            groups[g].blocks = 0u;
            ++used;
        }
        groups[g].bytes += entry->bytes;
        ++groups[g].blocks;
    }
    const unsigned untracked = s_spill_untracked;
    arena_unlock(arena);
    ESP_LOGW("h2_arena",
             "H2_ARENA_SPILL_LIVE phase=%s bytes=%u blocks=%u groups=%u "
             "other=%u untracked=%u",
             phase != NULL ? phase : "-", (unsigned)total, (unsigned)blocks,
             used, (unsigned)other, untracked);
    for (unsigned rank = 0u; rank < 12u && rank < used; ++rank) {
        unsigned best = rank;
        for (unsigned g = rank + 1u; g < used; ++g)
            if (groups[g].bytes > groups[best].bytes)
                best = g;
        if (best != rank) {
            __typeof__(groups[0]) swap = groups[rank];
            groups[rank] = groups[best];
            groups[best] = swap;
        }
        ESP_LOGW("h2_arena",
                 "H2_ARENA_SPILL_SITE rank=%u bytes=%u blocks=%u "
                 "pc=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
                 rank, (unsigned)groups[rank].bytes,
                 (unsigned)groups[rank].blocks, (unsigned)groups[rank].pc[0],
                 (unsigned)groups[rank].pc[1], (unsigned)groups[rank].pc[2],
                 (unsigned)groups[rank].pc[3], (unsigned)groups[rank].pc[4],
                 (unsigned)groups[rank].pc[5]);
    }
#else
    (void)arena;
    (void)phase;
    (void)force;
#endif
}

h2_pal_result_t h2_esp_platform_arena_destroy(h2_esp_platform_arena_t *arena) {
    if (arena == NULL)
        return H2_PAL_OK;
    const h2_pal_result_t rc = h2_mem_arena_destroy(arena->core);
    if (rc != H2_PAL_OK)
        return rc;
    vSemaphoreDelete(arena->lock);
    heap_caps_free(arena->block);
    heap_caps_free(arena);
    return H2_PAL_OK;
}
