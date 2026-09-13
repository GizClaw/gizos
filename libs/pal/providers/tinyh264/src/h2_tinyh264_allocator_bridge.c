#include "h2_tinyh264_allocator_bridge.h"
#include "h2_tinyh264_allocator_scope.h"

#include "h2/pal/os/h2_pal_mem.h"

#if defined(H2_TINYH264_SCOPE_TASK)
#include <stdint.h>
#include <stdatomic.h>

/* Supplied by the target SDK port; no SDK headers enter portable compilation.
 * The task identity remains stable until all of its stack scopes have left. */
extern const void *H2_TINYH264_SCOPE_TASK(void);
extern void H2_TINYH264_SCOPE_YIELD(uint32_t delay_ms);
static atomic_uint g_scope_lock;
static h2_tinyh264_allocator_scope_t *g_scopes;

static void scope_lock(void) {
    unsigned expected = 0u;
    while (!atomic_compare_exchange_weak_explicit(
        &g_scope_lock, &expected, 1u, memory_order_acquire, memory_order_relaxed)) {
        H2_TINYH264_SCOPE_YIELD(1u);
        expected = 0u;
    }
}
static void scope_unlock(void) {
    atomic_store_explicit(&g_scope_lock, 0u, memory_order_release);
}
static const h2_pal_mem_api_t *current_allocator(void) {
    const void *task = H2_TINYH264_SCOPE_TASK();
    const h2_pal_mem_api_t *allocator = NULL;
    scope_lock();
    for (h2_tinyh264_allocator_scope_t *node = g_scopes;
         node != NULL; node = node->next) {
        if (node->task == task) {
            allocator = node->allocator;
            break;
        }
    }
    scope_unlock();
    return allocator;
}
#else
static _Thread_local const h2_pal_mem_api_t *g_allocator;
static const h2_pal_mem_api_t *current_allocator(void) { return g_allocator; }
#endif

void h2_tinyh264_allocator_scope_enter(
    h2_tinyh264_allocator_scope_t *scope, const h2_pal_mem_api_t *allocator) {
#if defined(H2_TINYH264_SCOPE_TASK)
    scope->task = H2_TINYH264_SCOPE_TASK();
    scope->allocator = allocator;
    scope_lock();
    scope->next = g_scopes;
    g_scopes = scope;
    scope_unlock();
#else
    scope->previous = g_allocator;
    g_allocator = allocator;
#endif
}

void h2_tinyh264_allocator_scope_leave(h2_tinyh264_allocator_scope_t *scope) {
#if defined(H2_TINYH264_SCOPE_TASK)
    scope_lock();
    h2_tinyh264_allocator_scope_t **link = &g_scopes;
    while (*link != NULL && *link != scope) link = &(*link)->next;
    if (*link != NULL) *link = scope->next;
    scope_unlock();
#else
    g_allocator = scope->previous;
#endif
}

void *h2_tinyh264_malloc(size_t size) {
    return h2_pal_mem_alloc(current_allocator(), size);
}

void h2_tinyh264_free(void *ptr) {
    h2_pal_mem_free(current_allocator(), ptr);
}
