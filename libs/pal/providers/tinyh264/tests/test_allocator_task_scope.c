#define _POSIX_C_SOURCE 200809L
#include "h2_tinyh264_allocator_scope.h"
#include "h2_tinyh264_allocator_bridge.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static atomic_uint arrived;
typedef struct allocator { unsigned live; } allocator_t;
typedef struct allocation { allocator_t *owner; } allocation_t;

const void *test_scope_task(void) {
    static _Thread_local int identity;
    return &identity;
}
void test_scope_yield(uint32_t delay_ms) {
    (void)delay_ms;
    const struct timespec delay = {0, 100000L};
    (void)nanosleep(&delay, NULL);
}
static void *allocate(void *user, size_t size) {
    allocation_t *p = malloc(sizeof(*p) + size);
    assert(p != NULL);
    p->owner = user;
    ++p->owner->live;
    return p + 1;
}
static void release(void *user, void *ptr) {
    if (ptr == NULL) return;
    allocation_t *p = (allocation_t *)ptr - 1;
    assert(p->owner == user);
    assert(p->owner->live != 0u);
    --p->owner->live;
    free(p);
}
static const h2_pal_mem_vtable_t memory = {.alloc = allocate, .free = release};
static void barrier(unsigned epoch) {
    atomic_fetch_add(&arrived, 1u);
    while (atomic_load(&arrived) < epoch * 2u) test_scope_yield(1u);
}
static void *worker(void *user) {
    (void)user;
    allocator_t outer = {0}, inner = {0};
    const h2_pal_mem_api_t a = {.user = &outer, .vtable = &memory};
    const h2_pal_mem_api_t b = {.user = &inner, .vtable = &memory};
    for (unsigned i = 0u; i < 500u; ++i) {
        h2_tinyh264_allocator_scope_t first, second;
        h2_tinyh264_allocator_scope_enter(&first, &a);
        barrier(i * 2u + 1u);
        void *p = h2_tinyh264_malloc(16u);
        assert(p != NULL && outer.live == 1u);
        h2_tinyh264_allocator_scope_enter(&second, &b);
        void *q = h2_tinyh264_malloc(16u);
        assert(q != NULL && inner.live == 1u);
        h2_tinyh264_free(q);
        h2_tinyh264_allocator_scope_leave(&second);
        h2_tinyh264_free(p);
        h2_tinyh264_allocator_scope_leave(&first);
        /* The other task may still have an active scope. Never borrow it. */
        assert(h2_tinyh264_malloc(1u) == NULL);
        assert(outer.live == 0u && inner.live == 0u);
        barrier(i * 2u + 2u);
    }
    return NULL;
}
int main(void) {
    pthread_t first, second;
    assert(pthread_create(&first, NULL, worker, NULL) == 0);
    assert(pthread_create(&second, NULL, worker, NULL) == 0);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    assert(h2_tinyh264_malloc(1u) == NULL);
    return 0;
}
