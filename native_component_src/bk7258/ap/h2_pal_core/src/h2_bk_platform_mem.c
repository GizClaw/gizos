#include "h2_bk_platform_core.h"
#include "h2_bk_resource_stats_internal.h"

#include <os/mem.h>
#if defined(H2_BK_MEM_DIAGNOSTICS) && H2_BK_MEM_DIAGNOSTICS
#include <os/os.h>
#include <stdio.h>
typedef struct allocation_row {void *ptr, *caller; size_t bytes; unsigned generation;} allocation_row_t;
static allocation_row_t live[128], baseline[128];
static unsigned generation, overflow;
static void track(void *ptr, size_t bytes, void *caller) {
    if (ptr == NULL) return;
    uint32_t level = rtos_enter_critical();
    unsigned i = 0u;
    while (i < 128u && live[i].ptr != NULL) ++i;
    if (i == 128u) ++overflow;
    else live[i] = (allocation_row_t){ptr, caller, bytes, ++generation};
    rtos_exit_critical(level);
}
static void untrack(void *ptr) {
    if (ptr == NULL) return;
    uint32_t level = rtos_enter_critical();
    unsigned i = 0u;
    while (i < 128u && live[i].ptr != ptr) ++i;
    if (i == 128u) ++overflow;
    else live[i].ptr = NULL;
    rtos_exit_critical(level);
}
void h2_bk_mqtt_mem_mark(void) {
    uint32_t level = rtos_enter_critical();
    for (unsigned i = 0u; i < 128u; ++i) baseline[i] = live[i];
    rtos_exit_critical(level);
}
void h2_bk_mqtt_mem_report(void) {
    allocation_row_t current[128];
    uint32_t level = rtos_enter_critical();
    for (unsigned i = 0u; i < 128u; ++i) current[i] = live[i];
    unsigned errors = overflow;
    rtos_exit_critical(level);
    printf("H2_PAL_MQTT_MEMORY_JOURNAL errors=%u\r\n", errors);
    for (unsigned side = 0u; side < 2u; ++side) {
        const allocation_row_t *from = side ? current : baseline, *to = side ? baseline : current;
        for (unsigned i = 0u; i < 128u; ++i) {
            if (from[i].ptr == NULL) continue;
            unsigned j = 0u;
            while (j < 128u && (to[j].ptr != from[i].ptr || to[j].generation != from[i].generation)) ++j;
            if (j == 128u) printf("H2_PAL_MQTT_MEMORY_DELTA side=%s ptr=%p bytes=%zu caller=%p generation=%u\r\n",
                side ? "added" : "released", from[i].ptr, from[i].bytes, from[i].caller, from[i].generation);
        }
    }
    fflush(stdout);
}
#else
#define track(ptr, bytes, caller) ((void)0)
#define untrack(ptr) ((void)0)
#endif

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
    track(ptr, ptr != NULL ? xPortPointerSize(ptr) : 0u, __builtin_return_address(0));
    return ptr;
}

static void *bk_platform_realloc(void *user, void *ptr, size_t len) {
    const h2_bk_heap_context_t *ctx = (const h2_bk_heap_context_t *)user;
    size_t before = ptr != NULL ? xPortPointerSize(ptr) : 0u;
    void *next = ctx->kind == H2_BK_HEAP_PSRAM ? psram_realloc(ptr, len) : os_realloc(ptr, len);
    if (next != NULL || len == 0u) {
        untrack(ptr);
        if (ptr != NULL) h2_bk_memory_release(before);
        if (next != NULL) h2_bk_memory_acquire(xPortPointerSize(next));
        track(next, next != NULL ? xPortPointerSize(next) : 0u, __builtin_return_address(0));
    }
    return next;
}

static void bk_platform_free(void *user, void *ptr) {
    (void)user;
    if (ptr != NULL) h2_bk_memory_release(xPortPointerSize(ptr));
    untrack(ptr);
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
