#include "h2_esp_platform_arena.h"
#include "esp_heap_caps.h"
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#endif
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#define H2_ESP_ARENA_SPILL_SLOTS 512u
#define H2_ESP_ARENA_SPILL_FRAMES 6u
#define H2_ESP_ARENA_SPILL_GROUPS 24u

typedef struct spill_entry {
    void *ptr;
    size_t bytes;
    uint32_t pc[H2_ESP_ARENA_SPILL_FRAMES];
} spill_entry_t;

typedef struct spill_group {
    size_t bytes;
    unsigned blocks;
    uint32_t pc[H2_ESP_ARENA_SPILL_FRAMES];
} spill_group_t;
#endif

struct h2_esp_platform_arena {
    h2_pal_mem_api_t watched;
    void *block;
    h2_mem_arena_t *core;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    h2_pal_mem_api_t fallback;
#if defined(ESP_PLATFORM)
    /* Each instance owns its PSRAM table; its core mutex guards all fields. */
    spill_entry_t *spills;
    uint64_t untracked_events;
    uint64_t spill_failures;
    uint64_t refusals;
    int64_t last_spill_log_us;
    bool spill_log_emitted;
#endif
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

#if defined(ESP_PLATFORM)
static void log_spill_failure(h2_esp_platform_arena_t *arena,
                              const char *op, size_t bytes) {
    if (arena->spill_failures != UINT64_MAX)
        ++arena->spill_failures;
    const uint64_t count = arena->spill_failures;
    if (count > 16u && count % 64u != 0u)
        return;
    ESP_LOGW("h2_arena",
             "H2_ARENA_SPILL_FAILED op=%s bytes=%zu count=%llu psram_free=%zu "
             "psram_largest=%zu internal_free=%zu internal_largest=%zu",
             op, bytes, (unsigned long long)count,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* Runtime frame walking is an Xtensa SDK facility. Other architectures still
 * track sizes and lifetimes, but report zero PCs instead of guessing frames. */
static __attribute__((noinline)) void
spill_capture(uint32_t pc[H2_ESP_ARENA_SPILL_FRAMES]) {
    memset(pc, 0, H2_ESP_ARENA_SPILL_FRAMES * sizeof(pc[0]));
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    esp_backtrace_frame_t frame = {0};
    /* IDF 6.0.3 esp_debug_helpers.h takes three output pointers here;
     * only esp_backtrace_get_next_frame below takes a frame pointer. */
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    for (unsigned depth = 0u; depth < H2_ESP_ARENA_SPILL_FRAMES; ++depth) {
        pc[depth] = esp_cpu_process_stack_pc(frame.pc);
        if (frame.next_pc == 0u || !esp_backtrace_get_next_frame(&frame))
            break;
    }
#endif
}

__attribute__((noinline)) h2_pal_result_t h2_esp_platform_arena_capture_site(
    void *user, uintptr_t *out_caller, uintptr_t *out_outer) {
    (void)user;
    if (out_caller == NULL || out_outer == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_caller = 0u;
    *out_outer = 0u;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    uint32_t pc[H2_ESP_ARENA_SPILL_FRAMES];
    spill_capture(pc);
    /* The walker, provider, census probe and census adapter occupy the first
     * four frames. Retain the following two for application attribution. */
    *out_caller = (uintptr_t)pc[4];
    *out_outer = (uintptr_t)pc[5];
    return *out_caller != 0u ? H2_PAL_OK : H2_PAL_ERR_UNSUPPORTED;
#else
    return H2_PAL_ERR_UNSUPPORTED;
#endif
}

static spill_entry_t *spill_find(h2_esp_platform_arena_t *arena, void *ptr) {
    for (unsigned i = 0u; arena->spills != NULL &&
                          i < H2_ESP_ARENA_SPILL_SLOTS; ++i)
        if (arena->spills[i].ptr == ptr)
            return &arena->spills[i];
    return NULL;
}

static void spill_track(h2_esp_platform_arena_t *arena, spill_entry_t *slot,
                         void *ptr, size_t bytes) {
    const bool fresh = slot == NULL;
    if (fresh)
        slot = spill_find(arena, NULL);
    if (slot == NULL) {
        if (arena->untracked_events != UINT64_MAX)
            ++arena->untracked_events;
        return;
    }
    slot->ptr = ptr;
    slot->bytes = bytes;
    if (fresh)
        spill_capture(slot->pc);
}
#endif

static void *psram_alloc(void *user, size_t bytes) {
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if defined(ESP_PLATFORM)
    h2_esp_platform_arena_t *arena = user;
    if (ptr == NULL)
        log_spill_failure(arena, "alloc", bytes);
    else
        spill_track(arena, NULL, ptr, bytes);
#else
    (void)user;
#endif
    return ptr;
}

static void *psram_realloc(void *user, void *ptr, size_t bytes) {
#if defined(ESP_PLATFORM)
    h2_esp_platform_arena_t *arena = user;
    /* Resolve the entry before realloc invalidates the old base pointer. */
    spill_entry_t *slot = spill_find(arena, ptr);
#endif
    void *moved = heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if defined(ESP_PLATFORM)
    if (moved == NULL)
        log_spill_failure(arena, "realloc", bytes);
    else
        spill_track(arena, slot, moved, bytes);
#else
    (void)user;
#endif
    return moved;
}

static void psram_free(void *user, void *ptr) {
#if defined(ESP_PLATFORM)
    spill_entry_t *slot = spill_find(user, ptr);
    if (slot != NULL)
        slot->ptr = NULL;
#else
    (void)user;
#endif
    heap_caps_free(ptr);
}

static const h2_pal_mem_vtable_t psram_vtable = {
    .alloc = psram_alloc, .realloc = psram_realloc, .free = psram_free,
};

static void log_refusal(h2_esp_platform_arena_t *arena, const char *op,
                        size_t bytes) {
#if defined(ESP_PLATFORM)
    arena_lock(arena);
    if (arena->refusals != UINT64_MAX)
        ++arena->refusals;
    const uint64_t count = arena->refusals;
    arena_unlock(arena);
    if (count > 24u && count % 64u != 0u)
        return;
    h2_mem_arena_stats_t stats;
    h2_mem_arena_pool_inspection_t pools[2] = {0};
    if (h2_mem_arena_stats(arena->core, &stats) != H2_PAL_OK ||
        h2_mem_arena_inspect(arena->core, pools) != H2_PAL_OK)
        return;
    ESP_LOGW("h2_arena",
             "H2_ARENA_REFUSED op=%s bytes=%zu count=%llu "
             "small_live=%zu small_free=%zu small_largest=%zu "
             "large_live=%zu large_free=%zu large_largest=%zu "
             "psram_free=%zu psram_largest=%zu internal_free=%zu",
             op, bytes, (unsigned long long)count, stats.small.live_bytes,
             pools[0].free_bytes, pools[0].largest_free_block,
             stats.large.live_bytes, pools[1].free_bytes,
             pools[1].largest_free_block,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
    arena->fallback = (h2_pal_mem_api_t){.user = arena, .vtable = &psram_vtable};
    arena->block = heap_caps_malloc(config->reserved_bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (arena->block == NULL)
        goto unavailable;
#if defined(ESP_PLATFORM)
    if (config->spill_to_system) {
        arena->spills = heap_caps_malloc(
            H2_ESP_ARENA_SPILL_SLOTS * sizeof(*arena->spills),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (arena->spills != NULL)
            memset(arena->spills, 0,
                   H2_ESP_ARENA_SPILL_SLOTS * sizeof(*arena->spills));
    }
#endif
    arena->lock = xSemaphoreCreateMutexStatic(&arena->lock_storage);
    if (arena->lock == NULL)
        goto unavailable;
    const h2_mem_arena_config_t core_config = {
        .name = config->name,
        .block = arena->block,
        .block_bytes = config->reserved_bytes,
        .small_request_max = config->small_request_max,
        .small_pool_bytes = config->small_pool_bytes,
        .fallback = config->spill_to_system ? &arena->fallback : NULL,
        .lock = arena_lock,
        .unlock = arena_unlock,
        .lock_user = arena,
    };
    rc = h2_mem_arena_create(&core_config, &arena->core);
    if (rc != H2_PAL_OK)
        goto unavailable;
    arena->watched = (h2_pal_mem_api_t){.user = arena,
                                        .vtable = &watched_vtable};
    *out_arena = arena;
    return H2_PAL_OK;
unavailable:
    if (arena->lock != NULL)
        vSemaphoreDelete(arena->lock);
#if defined(ESP_PLATFORM)
    heap_caps_free(arena->spills);
#endif
    heap_caps_free(arena->block);
    heap_caps_free(arena);
    return rc;
}

const h2_pal_mem_api_t *
h2_esp_platform_arena_mem(h2_esp_platform_arena_t *arena) {
    return arena != NULL ? &arena->watched : NULL;
}

h2_mem_arena_t *h2_esp_platform_arena_core(h2_esp_platform_arena_t *arena) {
    return arena != NULL ? arena->core : NULL;
}

h2_pal_result_t
h2_esp_platform_arena_stats(h2_esp_platform_arena_t *arena,
                            h2_esp_platform_arena_stats_t *out_stats) {
    return h2_mem_arena_stats(arena != NULL ? arena->core : NULL, out_stats);
}

void h2_esp_platform_arena_log_spills(h2_esp_platform_arena_t *arena,
                                      const char *phase, bool force) {
#if defined(ESP_PLATFORM)
    if (arena == NULL)
        return;
    spill_group_t groups[H2_ESP_ARENA_SPILL_GROUPS];
    unsigned used = 0u, blocks = 0u;
    size_t total = 0u, other = 0u;
    arena_lock(arena);
    const int64_t now_us = esp_timer_get_time();
    if (!force && arena->spill_log_emitted &&
        now_us - arena->last_spill_log_us < INT64_C(60000000)) {
        arena_unlock(arena);
        return;
    }
    arena->last_spill_log_us = now_us;
    arena->spill_log_emitted = true;
    for (unsigned i = 0u; arena->spills != NULL &&
                          i < H2_ESP_ARENA_SPILL_SLOTS; ++i) {
        const spill_entry_t *entry = &arena->spills[i];
        if (entry->ptr == NULL)
            continue;
        total += entry->bytes;
        ++blocks;
        unsigned g = 0u;
        while (g < used && memcmp(groups[g].pc, entry->pc,
                                  sizeof(entry->pc)) != 0)
            ++g;
        if (g == used) {
            if (used == H2_ESP_ARENA_SPILL_GROUPS) {
                other += entry->bytes;
                continue;
            }
            groups[g] = (spill_group_t){0};
            memcpy(groups[g].pc, entry->pc, sizeof(entry->pc));
            ++used;
        }
        groups[g].bytes += entry->bytes;
        ++groups[g].blocks;
    }
    const uint64_t untracked = arena->untracked_events;
    arena_unlock(arena);
    /* Snapshot and group without allocating; SDK logging runs after unlock. */
    ESP_LOGW("h2_arena",
             "H2_ARENA_SPILL_LIVE phase=%s bytes=%zu blocks=%u groups=%u "
             "other=%zu untracked=%llu",
             phase != NULL ? phase : "-", total, blocks, used, other,
             (unsigned long long)untracked);
    for (unsigned rank = 0u; rank < used; ++rank) {
        unsigned best = rank;
        for (unsigned g = rank + 1u; g < used; ++g)
            if (groups[g].bytes > groups[best].bytes)
                best = g;
        if (best != rank) {
            const spill_group_t swap = groups[rank];
            groups[rank] = groups[best];
            groups[best] = swap;
        }
        ESP_LOGW("h2_arena",
                 "H2_ARENA_SPILL_SITE rank=%u bytes=%zu blocks=%u "
                 "pc=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
                 rank, groups[rank].bytes, groups[rank].blocks,
                 (unsigned)groups[rank].pc[0], (unsigned)groups[rank].pc[1],
                 (unsigned)groups[rank].pc[2], (unsigned)groups[rank].pc[3],
                 (unsigned)groups[rank].pc[4], (unsigned)groups[rank].pc[5]);
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
#if defined(ESP_PLATFORM)
    heap_caps_free(arena->spills);
#endif
    heap_caps_free(arena->block);
    heap_caps_free(arena);
    return H2_PAL_OK;
}
