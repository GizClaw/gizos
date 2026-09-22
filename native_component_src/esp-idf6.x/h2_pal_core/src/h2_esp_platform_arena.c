#include "h2_esp_platform_arena.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

struct h2_esp_platform_arena {
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

static void *psram_alloc(void *user, size_t bytes) {
    (void)user;
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *psram_realloc(void *user, void *ptr, size_t bytes) {
    (void)user;
    return heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void psram_free(void *user, void *ptr) {
    (void)user;
    heap_caps_free(ptr);
}

static const h2_pal_mem_vtable_t psram_vtable = {
    .alloc = psram_alloc, .realloc = psram_realloc, .free = psram_free,
};
static const h2_pal_mem_api_t psram_mem = {.vtable = &psram_vtable};

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
    arena->block = psram_alloc(NULL, config->reserved_bytes);
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
        .fallback = &psram_mem,
        .lock = arena_lock,
        .unlock = arena_unlock,
        .lock_user = arena,
    };
    rc = h2_mem_arena_create(&core_config, &arena->core);
    if (rc != H2_PAL_OK)
        goto unavailable;
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
    return h2_mem_arena_mem(arena != NULL ? arena->core : NULL);
}

h2_pal_result_t
h2_esp_platform_arena_stats(h2_esp_platform_arena_t *arena,
                            h2_esp_platform_arena_stats_t *out_stats) {
    return h2_mem_arena_stats(arena != NULL ? arena->core : NULL, out_stats);
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
