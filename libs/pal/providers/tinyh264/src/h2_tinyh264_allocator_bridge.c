#include "h2_tinyh264_allocator_bridge.h"

#include "h2/pal/os/h2_pal_mem.h"

#if defined(H2_TINYH264_SINGLE_THREADED_ALLOCATOR_SCOPE)
/*
 * JieLi's pi32v2 toolchain lowers C thread-local storage through compiler-rt's
 * emutls runtime. Its first access calls the SDK pthread_once(), which is an
 * unsupported assertion stub on WL82. TinyH264 calls are serialized by the
 * JieLi decoder task, so a process-global allocator scope is sufficient there.
 */
static const h2_pal_mem_api_t *g_allocator;
#else
static _Thread_local const h2_pal_mem_api_t *g_allocator;
#endif

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
