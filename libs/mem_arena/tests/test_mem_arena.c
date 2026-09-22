#include "h2_mem_arena.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_BYTES (512u * 1024u)
#define SMALL_BYTES (128u * 1024u)
#define SMALL_MAX (16u * 1024u)

/* Exact base tracking catches forwarding an interior/TLSF pointer to fallback.
 * Moving realloc deliberately alternates the base alignment to exercise the
 * arena's payload realignment as well as data preservation. */
static struct { void *base; void *raw; size_t bytes; } blocks[128];
static size_t block_count;
static unsigned alloc_calls;
static bool fail_heap;
static bool locked;
static unsigned lock_count;

static void lock(void *user) {
    assert(user == &locked && !locked);
    locked = true;
    ++lock_count;
}
static void unlock(void *user) {
    assert(user == &locked && locked);
    locked = false;
}
static size_t find(void *ptr) {
    for (size_t i = 0; i < block_count; ++i)
        if (blocks[i].base == ptr)
            return i;
    assert(false);
    return 0;
}
static void *alloc(void *user, size_t bytes) {
    assert(user == &block_count && locked);
    if (fail_heap)
        return NULL;
    void *raw = malloc(bytes + 32u);
    assert(raw != NULL && block_count < 128u);
    void *base = (unsigned char *)raw + (alloc_calls++ % 2u) * 8u;
    blocks[block_count].base = base;
    blocks[block_count].raw = raw;
    blocks[block_count++].bytes = bytes;
    return base;
}
static void free_block(void *user, void *ptr) {
    assert(user == &block_count && locked);
    const size_t i = find(ptr);
    free(blocks[i].raw);
    blocks[i] = blocks[--block_count];
}
static void *realloc_block(void *user, void *ptr, size_t bytes) {
    const size_t old_bytes = blocks[find(ptr)].bytes;
    void *next = alloc(user, bytes);
    if (next != NULL) {
        memcpy(next, ptr, old_bytes < bytes ? old_bytes : bytes);
        free_block(user, ptr);
    }
    return next;
}
static const h2_pal_mem_vtable_t vtable = {alloc, realloc_block, free_block};
static const h2_pal_mem_api_t fallback = {&block_count, &vtable};
static void *region;

static h2_mem_arena_config_t config(bool split, bool with_fallback) {
    return (h2_mem_arena_config_t){
        .name = "test", .block = region, .block_bytes = BLOCK_BYTES,
        .small_request_max = SMALL_MAX,
        .small_pool_bytes = split ? SMALL_BYTES : 0u,
        .fallback = with_fallback ? &fallback : NULL,
        .lock = lock, .unlock = unlock, .lock_user = &locked,
    };
}
static h2_mem_arena_t *create(bool split, bool with_fallback) {
    assert(block_count == 0u && !locked);
    region = malloc(BLOCK_BYTES);
    assert(region != NULL);
    h2_mem_arena_config_t cfg = config(split, with_fallback);
    h2_mem_arena_t *arena = NULL;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_OK && arena != NULL);
    return arena;
}
static h2_mem_arena_stats_t stats(h2_mem_arena_t *arena) {
    h2_mem_arena_stats_t value;
    unsigned before = lock_count;
    assert(h2_mem_arena_stats(arena, &value) == H2_PAL_OK);
    assert(lock_count == before + 1u && !locked);
    return value;
}
static void finish(h2_mem_arena_t *arena) {
    h2_mem_arena_stats_t s = stats(arena);
    assert(s.small.live_bytes == 0u && s.large.live_bytes == 0u);
    assert(s.fallback_live_bytes == 0u && block_count == 0u);
    assert(h2_mem_arena_destroy(arena) == H2_PAL_OK && !locked);
    free(region);
    region = NULL;
}
static bool inside(const void *p, size_t offset, size_t bytes) {
    const uintptr_t start = (uintptr_t)region + offset;
    return (uintptr_t)p >= start && (uintptr_t)p - start < bytes;
}
static void check(const void *ptr, size_t n, unsigned char value) {
    assert(ptr != NULL);
#if !defined(_MSC_VER)
    assert((uintptr_t)ptr % _Alignof(max_align_t) == 0u);
#endif
    for (size_t i = 0u; i < n; ++i)
        assert(((const unsigned char *)ptr)[i] == value);
}

static void test_routing_and_stats(void) {
    h2_mem_arena_t *arena = create(true, true);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    void *small = h2_pal_mem_alloc(mem, SMALL_MAX);
    void *large = h2_pal_mem_alloc(mem, SMALL_MAX + 1u);
    assert(inside(small, 0u, SMALL_BYTES));
    assert(inside(large, SMALL_BYTES, BLOCK_BYTES - SMALL_BYTES));
    h2_mem_arena_stats_t s = stats(arena);
    assert(s.reserved_bytes == BLOCK_BYTES);
    assert(s.small.reserved_bytes == SMALL_BYTES);
    assert(s.large.reserved_bytes == BLOCK_BYTES - SMALL_BYTES);
    assert(s.small.live_bytes == SMALL_MAX && s.small.peak_bytes == SMALL_MAX);
    assert(s.large.live_bytes == SMALL_MAX + 1u);
    assert(s.small.largest_request == SMALL_MAX);
    assert(s.large.largest_request == SMALL_MAX + 1u);
    assert(h2_mem_arena_destroy(arena) == H2_PAL_ERR_INVALID_STATE);
    h2_pal_mem_free(mem, small);
    h2_pal_mem_free(mem, large);
    assert(stats(arena).small.peak_bytes == SMALL_MAX);
    finish(arena);
}

static void test_no_borrowing(bool with_fallback) {
    h2_mem_arena_t *arena = create(true, with_fallback);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    void *small[32];
    size_t count = 0u;
    do {
        assert(count < 32u);
        small[count++] = h2_pal_mem_alloc(mem, SMALL_MAX);
    } while (inside(small[count - 1u], 0u, SMALL_BYTES));
    assert(!inside(small[count - 1u], SMALL_BYTES, BLOCK_BYTES - SMALL_BYTES));
    assert((small[count - 1u] != NULL) == with_fallback);
    h2_mem_arena_stats_t s = stats(arena);
    assert(s.small.fallback_count == 1u && s.small.fallback_bytes == SMALL_MAX);
    assert(s.large.live_bytes == 0u && s.large.fallback_count == 0u);
    /* Churn a full small pool without consuming space for a large buffer. */
    void *large = h2_pal_mem_alloc(mem, 256u * 1024u);
    assert(inside(large, SMALL_BYTES, BLOCK_BYTES - SMALL_BYTES));
    for (size_t i = 0; i < count; ++i)
        h2_pal_mem_free(mem, small[i]);
    h2_pal_mem_free(mem, large);
    large = h2_pal_mem_alloc(mem, BLOCK_BYTES - SMALL_BYTES);
    assert(!inside(large, 0u, BLOCK_BYTES));
    assert((large != NULL) == with_fallback);
    s = stats(arena);
    assert(s.small.live_bytes == 0u && s.large.live_bytes == 0u);
    assert(s.large.fallback_count == 1u);
    assert(s.large.fallback_bytes == BLOCK_BYTES - SMALL_BYTES);
    if (with_fallback) {
        assert(s.fallback_live_bytes == BLOCK_BYTES - SMALL_BYTES);
        assert(h2_mem_arena_destroy(arena) == H2_PAL_ERR_INVALID_STATE);
    }
    h2_pal_mem_free(mem, large);
    finish(arena);
}

static void test_realloc(void) {
    h2_mem_arena_t *arena = create(true, true);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    void *p = h2_pal_mem_realloc(mem, NULL, 101u);
    memset(p, 0xa5, 101u);
    assert(h2_pal_mem_realloc(mem, p, 1024u) == p);
    check(p, 101u, 0xa5);
    void *guard = h2_pal_mem_alloc(mem, 256u);
    void *moved = h2_pal_mem_realloc(mem, p, SMALL_MAX);
    assert(moved != NULL && moved != p);
    p = moved;
    check(p, 101u, 0xa5);
    p = h2_pal_mem_realloc(mem, p, SMALL_MAX + 1u);
    assert(inside(p, SMALL_BYTES, BLOCK_BYTES - SMALL_BYTES));
    check(p, 101u, 0xa5);
    assert(stats(arena).small.live_bytes == 256u);
    /* Grow in place in the large pool even though a second block cannot fit. */
    assert(h2_pal_mem_realloc(mem, p, 300u * 1024u) == p);
    check(p, 101u, 0xa5);
    fail_heap = true;
    assert(h2_pal_mem_realloc(mem, p, BLOCK_BYTES) == NULL);
    assert(h2_pal_mem_realloc(mem, p, SIZE_MAX) == NULL);
    check(p, 101u, 0xa5);
    assert(stats(arena).large.live_bytes == 300u * 1024u);
    fail_heap = false;
    p = h2_pal_mem_realloc(mem, p, BLOCK_BYTES);
    check(p, 101u, 0xa5);
    assert(!inside(p, 0u, BLOCK_BYTES));
    assert(stats(arena).fallback_live_bytes == BLOCK_BYTES);
    fail_heap = true;
    assert(h2_pal_mem_realloc(mem, p, BLOCK_BYTES * 2u) == NULL);
    check(p, 101u, 0xa5);
    fail_heap = false;
    p = h2_pal_mem_realloc(mem, p, BLOCK_BYTES * 2u);
    check(p, 101u, 0xa5);
    p = h2_pal_mem_realloc(mem, p, 100u * 1024u);
    assert(inside(p, SMALL_BYTES, BLOCK_BYTES - SMALL_BYTES));
    check(p, 101u, 0xa5);
    p = h2_pal_mem_realloc(mem, p, 101u);
    assert(inside(p, 0u, SMALL_BYTES));
    check(p, 101u, 0xa5);
    h2_mem_arena_stats_t s = stats(arena);
    assert(s.small.live_bytes == 357u && s.large.live_bytes == 0u);
    assert(s.fallback_live_bytes == 0u && block_count == 0u);
    assert(s.small.peak_bytes == SMALL_MAX + 256u);
    assert(s.large.peak_bytes == 300u * 1024u);
    assert(s.large.fallback_count == 4u);
    assert(s.large.fallback_bytes == 6u * BLOCK_BYTES);
    h2_pal_mem_free(mem, guard);
    assert(h2_pal_mem_realloc(mem, p, 0u) == NULL);
    assert(h2_pal_mem_alloc(mem, 0u) == NULL);
    h2_pal_mem_free(mem, NULL);
    finish(arena);
}

static void test_fallback_without_realloc(void) {
    h2_mem_arena_t *arena = create(true, false);
    assert(h2_mem_arena_destroy(arena) == H2_PAL_OK);
    const h2_pal_mem_vtable_t alloc_free = {alloc, NULL, free_block};
    const h2_pal_mem_api_t mem_fallback = {&block_count, &alloc_free};
    h2_mem_arena_config_t cfg = config(true, false);
    cfg.fallback = &mem_fallback;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_OK);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    void *p = h2_pal_mem_alloc(mem, BLOCK_BYTES);
    assert(p != NULL);
    memset(p, 0x3c, 128u);
    fail_heap = true;
    assert(h2_pal_mem_realloc(mem, p, 2u * BLOCK_BYTES) == NULL);
    check(p, 128u, 0x3c);
    fail_heap = false;
    p = h2_pal_mem_realloc(mem, p, 2u * BLOCK_BYTES);
    check(p, 128u, 0x3c);
    assert(block_count == 1u);
    h2_pal_mem_free(mem, p);
    finish(arena);
}

static void test_class_change_failure(void) {
    h2_mem_arena_t *arena = create(true, false);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    void *p = h2_pal_mem_alloc(mem, 128u);
    memset(p, 0x3c, 128u);
    void *full = h2_pal_mem_alloc(mem, 360u * 1024u);
    assert(full != NULL);
    assert(h2_pal_mem_realloc(mem, p, 32u * 1024u) == NULL);
    check(p, 128u, 0x3c);
    assert(stats(arena).small.live_bytes == 128u);
    h2_pal_mem_free(mem, full);
    h2_pal_mem_free(mem, p);
    finish(arena);
}

static void test_single_pool(void) {
    h2_mem_arena_t *arena = create(false, false);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    void *p = h2_pal_mem_alloc(mem, 64u);
    memset(p, 0x5a, 64u);
    assert(h2_pal_mem_realloc(mem, p, 400u * 1024u) == p);
    check(p, 64u, 0x5a);
    h2_mem_arena_stats_t s = stats(arena);
    assert(s.small.reserved_bytes == 0u && s.small.live_bytes == 0u);
    assert(s.large.reserved_bytes == BLOCK_BYTES && s.large.fallback_count == 0u);
    h2_pal_mem_free(mem, p);
    finish(arena);
}

static void test_invalid_config(void) {
    region = malloc(BLOCK_BYTES);
    assert(region != NULL);
    h2_mem_arena_config_t cfg = config(true, true);
    h2_mem_arena_t *arena = (h2_mem_arena_t *)region;
    assert(h2_mem_arena_create(NULL, &arena) == H2_PAL_ERR_INVALID_ARG);
    assert(arena == NULL);
    assert(h2_mem_arena_create(&cfg, NULL) == H2_PAL_ERR_INVALID_ARG);
    cfg.block = (unsigned char *)region + 1u;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    cfg = config(true, true); cfg.small_pool_bytes = 1u;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    cfg.small_pool_bytes = BLOCK_BYTES - 8u;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    cfg.small_pool_bytes = BLOCK_BYTES;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    cfg = config(false, true); cfg.block_bytes = 1u;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    cfg.block_bytes = SIZE_MAX;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    cfg = config(true, true); cfg.lock = NULL;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    cfg = config(true, true); cfg.unlock = NULL;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    const h2_pal_mem_api_t invalid = {0};
    cfg = config(true, true); cfg.fallback = &invalid;
    assert(h2_mem_arena_create(&cfg, &arena) == H2_PAL_ERR_INVALID_ARG);
    h2_mem_arena_stats_t s;
    memset(&s, 0xff, sizeof(s));
    assert(h2_mem_arena_stats(NULL, &s) == H2_PAL_ERR_INVALID_STATE);
    assert(s.reserved_bytes == 0u && s.small.live_bytes == 0u);
    assert(h2_mem_arena_stats(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_mem_arena_mem(NULL) == NULL);
    assert(h2_mem_arena_destroy(NULL) == H2_PAL_OK);
    free(region);
    region = NULL;
}

static void test_inspection(void) {
    h2_mem_arena_t *arena = create(true, true);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    h2_mem_arena_pool_inspection_t initial[2], used[2];
    h2_mem_arena_block_info_t info;
    assert(h2_mem_arena_inspect(arena, initial) == H2_PAL_OK);
    assert(initial[0].free_bytes == initial[0].largest_free_block);
    assert(initial[1].free_bytes == initial[1].largest_free_block);
    void *a = h2_pal_mem_alloc(mem, 1000);
    void *guard = h2_pal_mem_alloc(mem, 2000);
    void *b = h2_pal_mem_alloc(mem, 3000);
    assert(h2_mem_arena_block_info(arena, a, &info) == H2_PAL_OK);
    assert(info.requested_bytes == 1000 && info.consumed_bytes > 1000);
    assert(info.pool == 0 && !info.fallback);
    assert(h2_mem_arena_inspect(arena, used) == H2_PAL_OK);
    assert(used[0].consumed_bytes > 6000 && used[1].consumed_bytes == 0);
    h2_pal_mem_free(mem, a);
    assert(h2_mem_arena_inspect(arena, used) == H2_PAL_OK);
    assert(used[0].free_bytes > used[0].largest_free_block);
    h2_pal_mem_free(mem, b);
    h2_pal_mem_free(mem, guard);
    assert(h2_mem_arena_inspect(arena, used) == H2_PAL_OK);
    assert(used[0].free_bytes == initial[0].free_bytes);
    assert(used[0].consumed_bytes == 0);
    a = h2_pal_mem_alloc(mem, BLOCK_BYTES);
    assert(h2_mem_arena_block_info(arena, a, &info) == H2_PAL_OK);
    assert(info.fallback && info.pool == 1 && info.consumed_bytes > BLOCK_BYTES);
    assert(h2_mem_arena_inspect(arena, used) == H2_PAL_OK);
    assert(used[1].consumed_bytes == 0);
    h2_pal_mem_free(mem, a);
    assert(h2_mem_arena_inspect(arena, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_mem_arena_inspect(NULL, used) == H2_PAL_ERR_INVALID_ARG);
    assert(used[0].free_bytes == 0);
    assert(h2_mem_arena_block_info(arena, NULL, &info) == H2_PAL_ERR_INVALID_ARG);
    assert(info.requested_bytes == 0);
    assert(h2_mem_arena_block_info(arena, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    finish(arena);
}

static void test_exact_fit(size_t extra, bool grow, bool with_fallback) {
    const size_t request = 256u * 1024u;
    const size_t old_bytes = 32u * 1024u;
    h2_mem_arena_t *arena = create(true, with_fallback);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    h2_mem_arena_pool_inspection_t initial[2], fragmented[2], restored[2];
    assert(h2_mem_arena_inspect(arena, initial) == H2_PAL_OK);
    void *hole = h2_pal_mem_alloc(mem, request + extra);
    void *old = h2_pal_mem_alloc(mem, old_bytes);
    void *guard = h2_pal_mem_alloc(mem, old_bytes);
    assert(hole != NULL && old != NULL && guard != NULL);
    memset(old, 0xa5, old_bytes);
    memset(guard, 0x3c, old_bytes);
    h2_pal_mem_free(mem, hole);
    assert(h2_mem_arena_inspect(arena, fragmented) == H2_PAL_OK);
    /* The header/alignment allowance puts the request inside the 8 KiB
     * class starting at 256 KiB. Only the freed hole can satisfy it. */
    assert(fragmented[1].largest_free_block > request + extra);
    assert(fragmented[1].largest_free_block < request + 8192u);
    assert(fragmented[1].free_bytes > fragmented[1].largest_free_block);
    h2_mem_arena_stats_t before = stats(arena);
    assert(before.large.fallback_count == 0u);
    fail_heap = true;
    void *p = grow ? h2_pal_mem_realloc(mem, old, request)
                   : h2_pal_mem_alloc(mem, request);
    assert(p != NULL && p == hole);
    if (grow)
        check(p, old_bytes, 0xa5);
    memset(p, 0x5a, request);
    check(p, request, 0x5a);
    check(guard, old_bytes, 0x3c);
    h2_mem_arena_block_info_t info;
    assert(h2_mem_arena_block_info(arena, p, &info) == H2_PAL_OK);
    assert(info.pool == 1u && !info.fallback && info.requested_bytes == request);
    h2_mem_arena_stats_t after = stats(arena);
    assert(after.large.live_bytes == before.large.live_bytes + request -
                                        (grow ? old_bytes : 0u));
    assert(after.large.fallback_count == 0u && after.large.fallback_bytes == 0u);
    assert(after.fallback_live_bytes == 0u && block_count == 0u);
    fail_heap = false;
    h2_pal_mem_free(mem, p);
    if (!grow)
        h2_pal_mem_free(mem, old);
    h2_pal_mem_free(mem, guard);
    assert(h2_mem_arena_inspect(arena, restored) == H2_PAL_OK);
    assert(restored[1].free_bytes == initial[1].free_bytes);
    assert(restored[1].largest_free_block == initial[1].largest_free_block);
    finish(arena);
}

static void test_exact_fit_list(void) {
    const size_t request = 128u * 1024u + 2048u;
    const size_t old_bytes = 32u * 1024u;
    h2_mem_arena_t *arena = create(false, false);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    h2_mem_arena_pool_inspection_t initial[2], fragmented[2], restored[2];
    assert(h2_mem_arena_inspect(arena, initial) == H2_PAL_OK);
    void *hole = h2_pal_mem_alloc(mem, request + 512u);
    void *guard = h2_pal_mem_alloc(mem, old_bytes);
    void *smaller = h2_pal_mem_alloc(mem, request - 1024u);
    void *old = h2_pal_mem_alloc(mem, old_bytes);
    void *tail = h2_pal_mem_alloc(mem, 64u * 1024u);
    assert(hole != NULL && guard != NULL && smaller != NULL);
    assert(old != NULL && tail != NULL);
    memset(old, 0xa5, old_bytes);
    /* Both holes are in [128 KiB, 132 KiB). Free lists prepend entries,
     * so the insufficient block is encountered before the fitting block. */
    h2_pal_mem_free(mem, hole);
    h2_pal_mem_free(mem, smaller);
    assert(h2_mem_arena_inspect(arena, fragmented) == H2_PAL_OK);
    assert(fragmented[1].largest_free_block > request + 512u);
    assert(fragmented[1].largest_free_block < request + 1024u);
    h2_mem_arena_stats_t before = stats(arena);
    assert(before.large.fallback_count == 0u);
    /* No entry fits this larger request: reaching the sentinel must leave
     * the old block and free lists untouched. The smaller request then fits. */
    assert(h2_pal_mem_realloc(mem, old, request + 1024u) == NULL);
    check(old, old_bytes, 0xa5);
    assert(stats(arena).large.live_bytes == before.large.live_bytes);
    void *p = h2_pal_mem_realloc(mem, old, request);
    assert(p != NULL && p == hole);
    check(p, old_bytes, 0xa5);
    memset(p, 0x5a, request);
    check(p, request, 0x5a);
    h2_mem_arena_stats_t after = stats(arena);
    assert(after.large.live_bytes == before.large.live_bytes + request - old_bytes);
    assert(after.large.fallback_count == 1u);
    assert(after.large.fallback_bytes == request + 1024u);
    assert(after.fallback_live_bytes == 0u);
    h2_pal_mem_free(mem, p);
    h2_pal_mem_free(mem, guard);
    h2_pal_mem_free(mem, tail);
    assert(h2_mem_arena_inspect(arena, restored) == H2_PAL_OK);
    assert(restored[1].free_bytes == initial[1].free_bytes);
    assert(restored[1].largest_free_block == initial[1].largest_free_block);
    finish(arena);
}

int main(void) {
    test_routing_and_stats();
    test_no_borrowing(true);
    test_no_borrowing(false);
    test_realloc();
    test_fallback_without_realloc();
    test_class_change_failure();
    test_single_pool();
    test_invalid_config();
    test_inspection();
    for (unsigned extra = 0u; extra <= 512u; extra += 512u) {
        test_exact_fit(extra, false, false);
        test_exact_fit(extra, false, true);
        test_exact_fit(extra, true, false);
        test_exact_fit(extra, true, true);
    }
    test_exact_fit_list();
    return 0;
}
