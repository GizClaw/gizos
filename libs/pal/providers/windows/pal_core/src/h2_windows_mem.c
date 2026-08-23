#include "h2_windows_internal.h"

static void *windows_mem_alloc(void *user, size_t len) {
    h2_windows_platform_t *platform = user;
    void *memory = h2_windows_heap_alloc(len);
    if (memory != NULL) {
        (void)InterlockedIncrement(&platform->live_allocations);
    }
    return memory;
}

static void *windows_mem_realloc(void *user, void *memory, size_t len) {
    if (memory == NULL) {
        return windows_mem_alloc(user, len);
    }
    return h2_windows_heap_realloc(memory, len);
}

static void windows_mem_free(void *user, void *memory) {
    h2_windows_platform_t *platform = user;
    if (memory != NULL) {
        h2_windows_heap_free(memory);
        (void)InterlockedDecrement(&platform->live_allocations);
    }
}

const h2_pal_mem_vtable_t h2_windows_mem_vtable = {
    .alloc = windows_mem_alloc,
    .realloc = windows_mem_realloc,
    .free = windows_mem_free,
};
