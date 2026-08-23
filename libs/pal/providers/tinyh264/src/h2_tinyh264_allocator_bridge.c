#include "h2_tinyh264_allocator_bridge.h"

#include "h2/pal/os/h2_pal_mem.h"

static _Thread_local const h2_pal_mem_api_t *g_allocator;

const h2_pal_mem_api_t *h2_tinyh264_allocator_scope_enter(
    const h2_pal_mem_api_t *allocator) {
    const h2_pal_mem_api_t *previous = g_allocator;
    g_allocator = allocator;
    return previous;
}

void h2_tinyh264_allocator_scope_leave(
    const h2_pal_mem_api_t *previous) {
    g_allocator = previous;
}

void *h2_tinyh264_malloc(size_t size) {
    return h2_pal_mem_alloc(g_allocator, size);
}

void h2_tinyh264_free(void *ptr) {
    h2_pal_mem_free(g_allocator, ptr);
}
