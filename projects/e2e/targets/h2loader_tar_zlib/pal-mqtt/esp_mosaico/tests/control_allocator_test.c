#ifdef NDEBUG
#undef NDEBUG
#endif
#include "control_allocator.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void *blocks[16];
static size_t sizes[16], live, bytes;
static int locked, reject;
static void lock(void *u) { (void)u; assert(!locked); locked = 1; }
static void unlock(void *u) { (void)u; assert(locked); locked = 0; }
static size_t index_of(void *p) {
    for (size_t i = 0; i < 16; ++i) if (blocks[i] == p) return i;
    abort();
}
static size_t usable(void *p) { assert(locked); return sizes[index_of(p)]; }
static void *allocate(void *u, size_t n) {
    (void)u;
    if (reject || n == 0) return NULL;
    n = ((n + 15u) / 16u) * 16u;
    for (size_t i = 0; i < 16; ++i) if (!blocks[i]) {
        void *p = malloc(n);
        if (p) { blocks[i] = p; sizes[i] = n; ++live; bytes += n; }
        return p;
    }
    return NULL;
}
static void release(void *u, void *p) {
    (void)u;
    if (!p) return;
    size_t i = index_of(p); bytes -= sizes[i]; --live;
    free(p); blocks[i] = NULL; sizes[i] = 0;
}
static void *resize(void *u, void *p, size_t n) {
    if (reject) return NULL;
    size_t old = sizes[index_of(p)];
    void *next = allocate(u, n);
    if (next) { memcpy(next, p, old < n ? old : n); release(u, p); }
    return next;
}
static const h2_pal_mem_vtable_t vtable = {.alloc = allocate, .realloc = resize, .free = release};
static const h2_pal_mem_api_t backing = {.vtable = &vtable};
static void snapshot(h2_mqtt_control_allocator_t *s, size_t out[10]) {
    memset(out, 0, 10u * sizeof(size_t));
    lock(NULL); out[8] = live; out[9] = bytes;
    assert(h2_mqtt_control_filter(s, out) == H2_PAL_OK); unlock(NULL);
}
int main(void) {
    h2_mqtt_control_allocator_t scope = {.backing = &backing, .lock = lock,
        .unlock = unlock, .usable_size = usable};
    assert(h2_mqtt_control_init(NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_mqtt_control_init(&scope) == H2_PAL_OK);
    void *p = h2_pal_mem_alloc(&scope.api, 19);
    assert(scope.allocations == 1 && scope.bytes == 32);
    reject = 1;
    assert(h2_pal_mem_alloc(&scope.api, 64) == NULL);
    assert(h2_pal_mem_realloc(&scope.api, p, 65) == NULL);
    assert(scope.allocations == 1 && scope.bytes == 32 && live == 1 && bytes == 32);
    reject = 0; p = h2_pal_mem_realloc(&scope.api, p, 65);
    assert(p && scope.allocations == 1 && scope.bytes == 80);
    assert(h2_pal_mem_realloc(&scope.api, p, 0) == NULL);
    assert(scope.allocations == 0 && scope.bytes == 0 && live == 0);
    p = h2_pal_mem_realloc(&scope.api, NULL, 17);
    assert(p && scope.bytes == 32);
    size_t invalid[10] = {0};
    assert(h2_mqtt_control_filter(&scope, invalid) == H2_PAL_ERR_INVALID_STATE);
    h2_pal_mem_free(&scope.api, p); h2_pal_mem_free(&scope.api, NULL);
    assert(!scope.allocations && !scope.bytes && !locked);

    /* A management buffer closes while an App-owned allocation leaks. Raw
     * counts could falsely balance; filtered ownership must still expose it. */
    void *owner = h2_pal_mem_alloc(&backing, 512);
    p = h2_pal_mem_alloc(&scope.api, 128);
    size_t before[10], after[10]; snapshot(&scope, before);
    size_t raw_before = live;
    h2_pal_mem_free(&scope.api, p);
    void *leak = h2_pal_mem_alloc(&backing, 128);
    assert(live == raw_before); snapshot(&scope, after);
    assert(after[8] == before[8] + 1 && after[9] == before[9] + 128);
    h2_pal_mem_free(&backing, leak); snapshot(&scope, after);
    assert(memcmp(before, after, sizeof(before)) == 0);
    h2_pal_mem_free(&backing, owner);
    assert(live == 0 && bytes == 0 && !locked);
    return 0;
}
