#include "h2_tinyh264_allocator_bridge.h"
#include "h2_tinyh264_allocator_scope.h"

#include "h2/pal/os/h2_pal_mem.h"

#if defined(H2_TINYH264_SCOPE_TASK)
#include <stdint.h>
#include "h2_atomic.h"

/* Supplied by the target SDK port; no SDK headers enter portable compilation.
 * The task identity remains stable until all of its stack scopes have left. */
extern const void *H2_TINYH264_SCOPE_TASK(void);
extern void H2_TINYH264_SCOPE_YIELD(uint32_t delay_ms);
static h2_atomic_flag_t g_scope_lock = {0};
static h2_tinyh264_allocator_scope_t *g_scopes;
static bool g_scope_ready;

h2_pal_result_t h2_tinyh264_global_init(void) {
    if (g_scope_ready) return H2_PAL_ERR_INVALID_STATE;
    const h2_atomic_result_t rc = h2_atomic_flag_init(&g_scope_lock);
    if (rc == H2_ATOMIC_UNSUPPORTED) return H2_PAL_ERR_UNSUPPORTED;
    if (rc != H2_ATOMIC_OK) return H2_PAL_ERR_NO_MEMORY;
    g_scope_ready = true;
    return H2_PAL_OK;
}

h2_pal_result_t h2_tinyh264_global_shutdown(void) {
    if (!g_scope_ready) return H2_PAL_ERR_INVALID_STATE;
    if (g_scopes != NULL) return H2_PAL_ERR_BUSY;
    g_scope_ready = false;
    h2_atomic_flag_destroy(&g_scope_lock);
    return H2_PAL_OK;
}

bool h2_tinyh264_global_ready(void) { return g_scope_ready; }

static void scope_lock(void) {
    while (h2_atomic_flag_test_and_set(&g_scope_lock, H2_ATOMIC_ACQUIRE)) {
        H2_TINYH264_SCOPE_YIELD(1u);
    }
}
static void scope_unlock(void) {
    h2_atomic_flag_clear(&g_scope_lock, H2_ATOMIC_RELEASE);
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
h2_pal_result_t h2_tinyh264_global_init(void) { return H2_PAL_OK; }
h2_pal_result_t h2_tinyh264_global_shutdown(void) { return H2_PAL_OK; }
bool h2_tinyh264_global_ready(void) { return true; }
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
