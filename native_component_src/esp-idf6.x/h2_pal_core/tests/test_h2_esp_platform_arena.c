#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "h2_esp_platform_arena.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define RESERVATION (64u * 1024u)

/* Track exact SDK base pointers: an arena/interior pointer passed to the
 * system heap must fail, even if libc happens to accept it. */
static struct {
    void *ptr;
    size_t bytes;
    unsigned caps;
} blocks[32];
static size_t live_blocks;
static bool fail_psram;
static bool fail_internal;
static bool fail_mutex;
static void *reservation;
static unsigned mutex_count;
static bool locked;
static unsigned takes;

static size_t block_index(void *ptr) {
    for (size_t i = 0; i < live_blocks; ++i)
        if (blocks[i].ptr == ptr)
            return i;
    assert(false);
    return 0;
}

void *heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (((caps & MALLOC_CAP_SPIRAM) && fail_psram) ||
        ((caps & MALLOC_CAP_INTERNAL) && fail_internal))
        return NULL;
    void *ptr = malloc(bytes);
    assert(ptr != NULL && live_blocks < 32u);
    blocks[live_blocks].ptr = ptr;
    blocks[live_blocks].bytes = bytes;
    blocks[live_blocks++].caps = caps;
    if (bytes == RESERVATION)
        reservation = ptr;
    return ptr;
}

void heap_caps_free(void *ptr) {
    if (ptr == NULL)
        return;
    const size_t i = block_index(ptr);
    blocks[i] = blocks[--live_blocks];
    free(ptr);
}

void *heap_caps_realloc(void *ptr, size_t bytes, unsigned caps) {
    assert(locked);
    const size_t i = block_index(ptr);
    assert(blocks[i].caps == caps);
    /* Always move so preservation does not accidentally rely on libc. */
    void *next = heap_caps_malloc(bytes, caps);
    if (next == NULL)
        return NULL;
    memcpy(next, ptr, bytes < blocks[i].bytes ? bytes : blocks[i].bytes);
    heap_caps_free(ptr);
    return next;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
    if (fail_mutex)
        return NULL;
    ++mutex_count;
    return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
    assert(semaphore != NULL && timeout == portMAX_DELAY && !locked);
    locked = true;
    ++takes;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    assert(semaphore != NULL && locked);
    locked = false;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
    assert(semaphore != NULL && !locked && mutex_count == 1u);
    --mutex_count;
}

static bool in_arena(const void *ptr) {
    const uintptr_t p = (uintptr_t)ptr, start = (uintptr_t)reservation;
    return p >= start && p - start < RESERVATION;
}

static h2_esp_platform_arena_t *create(void) {
    const h2_esp_platform_arena_config_t config = {.reserved_bytes = RESERVATION, .name = "test"};
    h2_esp_platform_arena_t *arena = NULL;
    assert(h2_esp_platform_arena_create(&config, &arena) == H2_PAL_OK);
    assert(arena != NULL && live_blocks == 2u && mutex_count == 1u);
    return arena;
}

static h2_esp_platform_arena_stats_t snapshot(h2_esp_platform_arena_t *arena) {
    h2_esp_platform_arena_stats_t stats;
    const unsigned before = takes;
    assert(h2_esp_platform_arena_stats(arena, &stats) == H2_PAL_OK);
    assert(takes == before + 1u && !locked);
    return stats;
}

static void finish(h2_esp_platform_arena_t *arena) {
    assert(snapshot(arena).large.live_bytes == 0u);
    assert(snapshot(arena).fallback_live_bytes == 0u);
    assert(h2_esp_platform_arena_destroy(arena) == H2_PAL_OK);
    assert(live_blocks == 0u && mutex_count == 0u);
}

static void check_bytes(void *ptr, size_t n, unsigned char value) {
    assert(ptr != NULL);
    /* MSVC-safe alignment type, matching fundamental PAL object alignment. */
    typedef union {
        long double d;
        void *p;
        uint64_t i;
    } alignment_t;
    assert((uintptr_t)ptr % _Alignof(alignment_t) == 0u);
#if !defined(_MSC_VER)
    assert((uintptr_t)ptr % _Alignof(max_align_t) == 0u);
#endif
    for (size_t i = 0; i < n; ++i)
        assert(((unsigned char *)ptr)[i] == value);
}

static void test_arena(void) {
    h2_esp_platform_arena_t *arena = create();
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    void *a = h2_pal_mem_alloc(mem, 129u);
    void *guard = h2_pal_mem_alloc(mem, 64u);
    assert(in_arena(a) && in_arena(guard));
    memset(a, 0xa5, 129u);
    void *old = a;
    a = h2_pal_mem_realloc(mem, a, 8193u);
    assert(a != old && in_arena(a));
    check_bytes(a, 129u, 0xa5);
    a = h2_pal_mem_realloc(mem, a, 17u);
    check_bytes(a, 17u, 0xa5);
    h2_esp_platform_arena_stats_t stats = snapshot(arena);
    assert(stats.reserved_bytes == RESERVATION);
    assert(stats.large.live_bytes == 81u && stats.large.peak_bytes == 8257u);
    assert(stats.fallback_live_bytes == 0u && stats.large.fallback_count == 0u);
    assert(stats.large.largest_request == 8193u);
    assert(h2_esp_platform_arena_destroy(arena) == H2_PAL_ERR_INVALID_STATE);
    h2_pal_mem_free(mem, guard);
    assert(h2_pal_mem_realloc(mem, a, 0u) == NULL);
    assert(h2_pal_mem_alloc(mem, 0u) == NULL);
    assert(h2_pal_mem_alloc(mem, SIZE_MAX) == NULL);
    h2_pal_mem_free(mem, NULL);
    finish(arena);
}

/* Growing within a nearly full pool must reuse the owner's realloc, rather
 * than require a second large allocation and fall back unnecessarily. */
static void test_in_place_growth(void) {
    h2_esp_platform_arena_t *arena = create();
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    void *p = h2_pal_mem_alloc(mem, 24u * 1024u);
    memset(p, 0x5a, 24u * 1024u);
    void *next = h2_pal_mem_realloc(mem, p, 48u * 1024u);
    assert(next == p);
    check_bytes(next, 24u * 1024u, 0x5a);
    assert(snapshot(arena).large.fallback_count == 0u);
    h2_pal_mem_free(mem, next);
    finish(arena);
}

static void test_fallback_and_migration(void) {
    h2_esp_platform_arena_t *arena = create();
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    void *a = h2_pal_mem_alloc(mem, 128u);
    memset(a, 0x3c, 128u);
    fail_psram = true;
    assert(h2_pal_mem_realloc(mem, a, RESERVATION) == NULL);
    check_bytes(a, 128u, 0x3c);
    assert(snapshot(arena).large.live_bytes == 128u);
    assert(snapshot(arena).fallback_live_bytes == 0u);
    fail_psram = false;
    a = h2_pal_mem_realloc(mem, a, RESERVATION);
    assert(!in_arena(a));
    check_bytes(a, 128u, 0x3c);
    assert(snapshot(arena).large.live_bytes == 0u);
    assert(snapshot(arena).fallback_live_bytes == RESERVATION);
    assert(h2_esp_platform_arena_destroy(arena) == H2_PAL_ERR_INVALID_STATE);
    fail_psram = true;
    assert(h2_pal_mem_realloc(mem, a, 2u * RESERVATION) == NULL);
    check_bytes(a, 128u, 0x3c);
    fail_psram = false;
    a = h2_pal_mem_realloc(mem, a, 2u * RESERVATION);
    check_bytes(a, 128u, 0x3c);
    assert(!in_arena(a) && snapshot(arena).large.peak_bytes == 128u);
    a = h2_pal_mem_realloc(mem, a, 256u);
    assert(in_arena(a));
    check_bytes(a, 128u, 0x3c);
    h2_esp_platform_arena_stats_t stats = snapshot(arena);
    assert(stats.large.live_bytes == 256u && stats.large.peak_bytes == 256u);
    assert(stats.fallback_live_bytes == 0u && live_blocks == 2u);
    assert(stats.large.fallback_count == 4u &&
           stats.large.fallback_bytes == 6u * RESERVATION);
    assert(stats.large.largest_request == 2u * RESERVATION);
    h2_pal_mem_free(mem, a);
    /* Fill the pool, then force a small request to the fallback heap. */
    a = h2_pal_mem_alloc(mem, 48u * 1024u);
    void *b = h2_pal_mem_alloc(mem, 32u * 1024u);
    assert(in_arena(a) && !in_arena(b));
    assert(snapshot(arena).large.live_bytes == 48u * 1024u);
    assert(snapshot(arena).fallback_live_bytes == 32u * 1024u);
    h2_pal_mem_free(mem, b);
    h2_pal_mem_free(mem, a);
    finish(arena);
}

static void test_split_pools(void) {
    const h2_esp_platform_arena_config_t config = {
        .reserved_bytes = RESERVATION, .small_pool_bytes = RESERVATION / 2u,
        .small_request_max = 1024u,
    };
    h2_esp_platform_arena_t *arena = NULL;
    assert(h2_esp_platform_arena_create(&config, &arena) == H2_PAL_OK);
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    void *small = h2_pal_mem_alloc(mem, 1024u);
    void *large = h2_pal_mem_alloc(mem, 1025u);
    assert(in_arena(small) && in_arena(large));
    assert((uintptr_t)small - (uintptr_t)reservation < RESERVATION / 2u);
    assert((uintptr_t)large - (uintptr_t)reservation >= RESERVATION / 2u);
    h2_esp_platform_arena_stats_t stats = snapshot(arena);
    assert(stats.small.live_bytes == 1024u && stats.large.live_bytes == 1025u);
    assert(stats.small.reserved_bytes == RESERVATION / 2u);
    h2_pal_mem_free(mem, small);
    h2_pal_mem_free(mem, large);
    finish(arena);
}

static void test_failure(void) {
    h2_esp_platform_arena_config_t config = {.reserved_bytes = RESERVATION};
    h2_esp_platform_arena_t *arena = NULL;
    fail_psram = true;
    assert(h2_esp_platform_arena_create(&config, &arena) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(arena == NULL && live_blocks == 0u);
    fail_psram = false;
    fail_internal = true;
    assert(h2_esp_platform_arena_create(&config, &arena) ==
           H2_PAL_ERR_NO_MEMORY);
    fail_internal = false;
    fail_mutex = true;
    assert(h2_esp_platform_arena_create(&config, &arena) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(arena == NULL && live_blocks == 0u && mutex_count == 0u);
    fail_mutex = false;
    assert(h2_esp_platform_arena_create(NULL, &arena) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_platform_arena_create(&config, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    config.reserved_bytes = 1u;
    assert(h2_esp_platform_arena_create(&config, &arena) ==
           H2_PAL_ERR_INVALID_ARG);
    config.reserved_bytes = SIZE_MAX;
    assert(h2_esp_platform_arena_create(&config, &arena) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_esp_platform_arena_stats_t stats;
    memset(&stats, 0xff, sizeof(stats));
    assert(h2_esp_platform_arena_stats(NULL, &stats) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(stats.reserved_bytes == 0u && stats.fallback_live_bytes == 0u);
    assert(h2_esp_platform_arena_mem(NULL) == NULL);
    assert(h2_esp_platform_arena_destroy(NULL) == H2_PAL_OK);
    finish(create());
}

int main(void) {
    test_arena();
    test_in_place_growth();
    test_fallback_and_migration();
    test_split_pools();
    test_failure();
    return 0;
}
