#include "h2_esp_platform_core.h"

#include "esp_heap_caps.h"

typedef struct h2_esp_heap_context {
    uint32_t caps;
} h2_esp_heap_context_t;

static void *esp_platform_alloc(void *user, size_t len) {
    const h2_esp_heap_context_t *ctx = (const h2_esp_heap_context_t *)user;
    return heap_caps_malloc(len, ctx->caps);
}

static void *esp_platform_realloc(void *user, void *ptr, size_t len) {
    const h2_esp_heap_context_t *ctx = (const h2_esp_heap_context_t *)user;
    return heap_caps_realloc(ptr, len, ctx->caps);
}

static void esp_platform_free(void *user, void *ptr) {
    (void)user;
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
