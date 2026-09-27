#include "h2_esp_platform_core.h"
#include "h2_esp_resource_stats_internal.h"

#include "esp_heap_caps.h"

#include <string.h>

typedef struct h2_esp_heap_context {
    uint32_t caps;
} h2_esp_heap_context_t;

static void *esp_platform_alloc(void *user, size_t len) {
    const h2_esp_heap_context_t *ctx = (const h2_esp_heap_context_t *)user;
    void *ptr = heap_caps_aligned_alloc(_Alignof(max_align_t), len, ctx->caps);
    if (ptr != NULL) h2_esp_memory_acquire(heap_caps_get_allocated_size(ptr));
    return ptr;
}

static void *esp_platform_realloc(void *user, void *ptr, size_t len) {
    const h2_esp_heap_context_t *ctx = (const h2_esp_heap_context_t *)user;
    /* IDF realloc may move an aligned block to a merely 4-byte-aligned one.
     * Preserve the PAL malloc contract, including the old block on failure. */
    if (ptr == NULL) return esp_platform_alloc(user, len);
    size_t before = heap_caps_get_allocated_size(ptr);
    if (len == 0u) {
        h2_esp_memory_release(before);
        heap_caps_free(ptr);
        return NULL;
    }
    void *next = heap_caps_aligned_alloc(_Alignof(max_align_t), len, ctx->caps);
    if (next != NULL) {
        memcpy(next, ptr, before < len ? before : len);
        heap_caps_free(ptr);
        h2_esp_memory_release(before);
        h2_esp_memory_acquire(heap_caps_get_allocated_size(next));
    }
    return next;
}

static void esp_platform_free(void *user, void *ptr) {
    (void)user;
    if (ptr != NULL) h2_esp_memory_release(heap_caps_get_allocated_size(ptr));
    heap_caps_free(ptr);
}

static h2_pal_mem_api_t *esp_platform_allocator(uint32_t caps) {
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = esp_platform_alloc,
        .realloc = esp_platform_realloc,
        .free = esp_platform_free,
    };
    static h2_esp_heap_context_t contexts[4];
    static h2_pal_mem_api_t allocators[4];
    static int initialized;

    if (!initialized) {
        contexts[0].caps = MALLOC_CAP_8BIT;
        contexts[1].caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        contexts[2].caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        contexts[3].caps = MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
        for (size_t i = 0u; i < 4u; ++i) {
            allocators[i].user = &contexts[i];
            allocators[i].vtable = &vtable;
        }
        initialized = 1;
    }

    for (size_t i = 0u; i < 4u; ++i) {
        if (contexts[i].caps == caps) {
            return &allocators[i];
        }
    }
    return &allocators[0];
}

h2_pal_mem_api_t *h2_esp_platform_default_allocator(void) {
    return esp_platform_allocator(MALLOC_CAP_8BIT);
}

h2_pal_mem_api_t *h2_esp_platform_psram_allocator(void) {
    return esp_platform_allocator(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

h2_pal_mem_api_t *h2_esp_platform_internal_allocator(void) {
    return esp_platform_allocator(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

h2_pal_mem_api_t *h2_esp_platform_dma_allocator(void) {
    return esp_platform_allocator(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}
