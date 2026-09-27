#include "h2_bk_platform_core.h"
#include "h2_bk_resource_stats_internal.h"

#include <os/mem.h>

extern size_t xPortPointerSize(void *ptr);

typedef enum h2_bk_heap_kind {
    H2_BK_HEAP_DEFAULT = 0,
    H2_BK_HEAP_SRAM = 1,
    H2_BK_HEAP_PSRAM = 2,
} h2_bk_heap_kind_t;

typedef struct h2_bk_heap_context {
    h2_bk_heap_kind_t kind;
} h2_bk_heap_context_t;

static void *bk_platform_alloc(void *user, size_t len) {
    const h2_bk_heap_context_t *ctx = (const h2_bk_heap_context_t *)user;
    void *ptr = ctx->kind == H2_BK_HEAP_PSRAM ? psram_malloc(len) : os_malloc(len);
    if (ptr != NULL) h2_bk_memory_acquire(xPortPointerSize(ptr));
    return ptr;
}

static void *bk_platform_realloc(void *user, void *ptr, size_t len) {
    const h2_bk_heap_context_t *ctx = (const h2_bk_heap_context_t *)user;
    size_t before = ptr != NULL ? xPortPointerSize(ptr) : 0u;
    void *next = ctx->kind == H2_BK_HEAP_PSRAM ? psram_realloc(ptr, len) : os_realloc(ptr, len);
    if (next != NULL || len == 0u) {
        if (ptr != NULL) h2_bk_memory_release(before);
        if (next != NULL) h2_bk_memory_acquire(xPortPointerSize(next));
    }
    return next;
}

static void bk_platform_free(void *user, void *ptr) {
    (void)user;
    if (ptr != NULL) h2_bk_memory_release(xPortPointerSize(ptr));
    os_free(ptr);
}

static h2_pal_mem_api_t *bk_platform_allocator(h2_bk_heap_kind_t kind) {
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = bk_platform_alloc,
        .realloc = bk_platform_realloc,
        .free = bk_platform_free,
    };
    static h2_bk_heap_context_t contexts[3] = {
        { .kind = H2_BK_HEAP_DEFAULT },
        { .kind = H2_BK_HEAP_SRAM },
        { .kind = H2_BK_HEAP_PSRAM },
    };
    static h2_pal_mem_api_t allocators[3];
    static int initialized;

    if (!initialized) {
        for (size_t i = 0u; i < 3u; ++i) {
            allocators[i].user = &contexts[i];
            allocators[i].vtable = &vtable;
        }
        initialized = 1;
    }
    return &allocators[(int)kind];
}

h2_pal_mem_api_t *h2_bk_platform_default_allocator(void) {
    return bk_platform_allocator(H2_BK_HEAP_DEFAULT);
}

h2_pal_mem_api_t *h2_bk_platform_sram_allocator(void) {
    return bk_platform_allocator(H2_BK_HEAP_SRAM);
}

h2_pal_mem_api_t *h2_bk_platform_psram_allocator(void) {
    return bk_platform_allocator(H2_BK_HEAP_PSRAM);
}
