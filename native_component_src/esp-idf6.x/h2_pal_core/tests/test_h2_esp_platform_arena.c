#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "h2_esp_platform_arena.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include "esp_debug_helpers.h"
#include <stdarg.h>
#include <stdio.h>
#endif

#define RESERVATION (64u * 1024u)

/* Track exact SDK base pointers: an arena/interior pointer passed to the
 * system heap must fail, even if libc happens to accept it. */
static struct {
    void *ptr;
    size_t bytes;
    unsigned caps;
} blocks[1024];
static size_t live_blocks;
static bool fail_psram;
static bool fail_internal;
static bool fail_mutex;
static void *reservation;
static unsigned mutex_count;
static bool locked;
static unsigned takes;
static unsigned heap_calls;
#if defined(ESP_PLATFORM)
static bool capturing_census;
static bool fail_registry;
#define BASE_BLOCKS 3u
#else
#define BASE_BLOCKS 2u
#endif

static size_t block_index(void *ptr) {
    for (size_t i = 0; i < live_blocks; ++i)
        if (blocks[i].ptr == ptr)
            return i;
    assert(false);
    return 0;
}

void *heap_caps_malloc(size_t bytes, unsigned caps) {
    ++heap_calls;
#if defined(ESP_PLATFORM)
    if (fail_registry && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) &&
        bytes != RESERVATION)
        return NULL;
#endif
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (((caps & MALLOC_CAP_SPIRAM) && fail_psram) ||
        ((caps & MALLOC_CAP_INTERNAL) && fail_internal))
        return NULL;
    void *ptr = malloc(bytes);
    assert(ptr != NULL && live_blocks < 1024u);
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
    assert(semaphore != NULL && !locked && mutex_count > 0u);
    --mutex_count;
}

static bool in_arena(const void *ptr) {
    const uintptr_t p = (uintptr_t)ptr, start = (uintptr_t)reservation;
    return p >= start && p - start < RESERVATION;
}

static h2_esp_platform_arena_t *create(bool spill) {
    const h2_esp_platform_arena_config_t config = {
        .reserved_bytes = RESERVATION, .name = "test", .spill_to_system = spill,
    };
    h2_esp_platform_arena_t *arena = NULL;
    assert(h2_esp_platform_arena_create(&config, &arena) == H2_PAL_OK);
    assert(arena != NULL && live_blocks == (spill ? BASE_BLOCKS : 2u) && mutex_count == 1u);
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
    h2_esp_platform_arena_t *arena = create(false);
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    assert(h2_esp_platform_arena_core(arena) != NULL);
    assert(mem->user == arena && mem->user != h2_esp_platform_arena_core(arena));
    h2_mem_arena_pool_inspection_t pools[2];
    assert(h2_mem_arena_inspect(h2_esp_platform_arena_core(arena), pools) ==
           H2_PAL_OK);
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
    h2_esp_platform_arena_t *arena = create(false);
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
    h2_esp_platform_arena_t *arena = create(true);
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
    assert(stats.fallback_live_bytes == 0u && live_blocks == BASE_BLOCKS);
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

static void test_default_no_spill(void) {
    const h2_esp_platform_arena_config_t config = {.reserved_bytes = RESERVATION};
    h2_esp_platform_arena_t *arena = NULL;
    assert(h2_esp_platform_arena_create(&config, &arena) == H2_PAL_OK);
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    void *p = h2_pal_mem_alloc(mem, 128u);
    memset(p, 0x31, 128u);
    const unsigned initial_calls = heap_calls;
    assert(h2_pal_mem_alloc(mem, RESERVATION) == NULL);
    assert(h2_pal_mem_realloc(mem, p, RESERVATION) == NULL);
    check_bytes(p, 128u, 0x31);
    assert(heap_calls == initial_calls);
    h2_esp_platform_arena_stats_t stats = snapshot(arena);
    assert(stats.large.live_bytes == 128u && stats.fallback_live_bytes == 0u);
    assert(stats.large.fallback_count == 2u);
    h2_pal_mem_free(mem, p);
    h2_esp_platform_arena_log_spills(NULL, NULL, false);
    h2_esp_platform_arena_log_spills(arena, "no-spill", true);
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
    assert(h2_esp_platform_arena_core(NULL) == NULL);
    assert(h2_esp_platform_arena_destroy(NULL) == H2_PAL_OK);
    finish(create(false));
}

#if defined(ESP_PLATFORM)
static int64_t now_us;
static uint32_t caller_pc = 0x40001000u;
static unsigned captures;
static unsigned trace_frames = 6u;
static unsigned frame_steps;
static bool corrupt_frame;
static uint32_t logged_frames[6];
static unsigned summary_logs;
static unsigned failure_logs;
static unsigned refusal_logs;
static unsigned long long last_refusal_count;
static char last_refusal[512];
static unsigned site_logs;
static size_t logged_bytes, logged_other;
static unsigned logged_blocks, logged_groups;
static unsigned long long logged_untracked;
static uint32_t logged_pc;
static char last_failure[256];

int64_t esp_timer_get_time(void) { return now_us; }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 4096u; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 2048u; }

void esp_backtrace_get_start(uint32_t *pc, uint32_t *sp, uint32_t *next_pc) {
    assert(locked || capturing_census);
    ++captures;
    assert(pc != sp && pc != next_pc && sp != next_pc);
    *pc = caller_pc;
    *sp = 0u;
    *next_pc = trace_frames > 1u ? caller_pc + 4u : 0u;
}

bool esp_backtrace_get_next_frame(esp_backtrace_frame_t *frame) {
    ++frame_steps;
    if (corrupt_frame)
        return false;
    frame->pc = frame->next_pc;
    ++frame->sp;
    frame->next_pc = frame->sp + 1u < trace_frames ? frame->pc + 4u : 0u;
    return true;
}

void test_arena_log(const char *tag, const char *format, ...) {
    assert(strcmp(tag, "h2_arena") == 0);
    char line[512];
    va_list args;
    va_start(args, format);
    const int bytes = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    assert(bytes > 0 && (size_t)bytes < sizeof(line));
    if (strstr(line, "H2_ARENA_SPILL_LIVE") != NULL) {
        assert(!locked);
        ++summary_logs;
        assert(sscanf(line, "H2_ARENA_SPILL_LIVE phase=%*s bytes=%zu blocks=%u "
                      "groups=%u other=%zu untracked=%llu",
                      &logged_bytes, &logged_blocks, &logged_groups,
                      &logged_other, &logged_untracked) == 5);
        site_logs = 0u;
    } else if (strstr(line, "H2_ARENA_SPILL_SITE") != NULL) {
        assert(!locked);
        ++site_logs;
        unsigned pcs[6];
        assert(sscanf(line, "H2_ARENA_SPILL_SITE rank=%*u bytes=%*u blocks=%*u "
                      "pc=0x%x 0x%x 0x%x 0x%x 0x%x 0x%x", &pcs[0], &pcs[1],
                      &pcs[2], &pcs[3], &pcs[4], &pcs[5]) == 6);
        logged_pc = pcs[0];
        for (unsigned i = 0u; i < 6u; ++i)
            logged_frames[i] = pcs[i];
    } else if (strstr(line, "H2_ARENA_REFUSED") != NULL) {
        assert(!locked);
        ++refusal_logs;
        assert(strstr(line, "small_live=") != NULL);
        assert(strstr(line, "small_free=") != NULL);
        assert(strstr(line, "small_largest=") != NULL);
        assert(strstr(line, "large_live=") != NULL);
        assert(strstr(line, "large_free=") != NULL);
        assert(strstr(line, "large_largest=") != NULL);
        assert(strstr(line, "psram_free=4096 psram_largest=2048 "
                            "internal_free=4096") != NULL);
        assert(sscanf(line, "H2_ARENA_REFUSED op=%*s bytes=%*zu count=%llu",
                      &last_refusal_count) == 1);
        strcpy(last_refusal, line);
    } else {
        assert(locked && strstr(line, "H2_ARENA_SPILL_FAILED") != NULL);
        ++failure_logs;
        assert(strlen(line) < sizeof(last_failure));
        strcpy(last_failure, line);
    }
}

static void dump(h2_esp_platform_arena_t *arena) {
    const unsigned before = heap_calls;
    h2_esp_platform_arena_log_spills(arena, NULL, true);
    assert(heap_calls == before && site_logs == logged_groups);
}

static void test_spill_lifecycle(void) {
    h2_esp_platform_arena_t *arena = create(true);
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    caller_pc = 0x40001000u;
    void *a = h2_pal_mem_alloc(mem, RESERVATION);
    void *b = h2_pal_mem_alloc(mem, RESERVATION);
    assert(a != NULL && b != NULL);
    memset(a, 0x42, 128u);
    dump(arena);
    assert(logged_blocks == 2u && logged_groups == 1u);
    const size_t original_bytes = logged_bytes;
    assert(original_bytes > 2u * RESERVATION && logged_untracked == 0u);
    const unsigned before_captures = captures;
    caller_pc = 0x40002000u;
    a = h2_pal_mem_realloc(mem, a, 2u * RESERVATION);
    check_bytes(a, 128u, 0x42);
    dump(arena);
    assert(logged_blocks == 2u && logged_groups == 1u);
    assert(logged_bytes == original_bytes + RESERVATION);
    assert(captures == before_captures);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    assert(logged_pc == 0x40001000u);
#else
    assert(logged_pc == 0u);
#endif
    const unsigned before_failures = failure_logs;
    fail_psram = true;
    assert(h2_pal_mem_realloc(mem, a, 3u * RESERVATION) == NULL);
    assert(h2_pal_mem_alloc(mem, RESERVATION) == NULL);
    fail_psram = false;
    assert(failure_logs == before_failures + 2u);
    assert(strstr(last_failure, "op=alloc") != NULL);
    check_bytes(a, 128u, 0x42);
    dump(arena);
    assert(logged_bytes == original_bytes + RESERVATION && logged_blocks == 2u);
    a = h2_pal_mem_realloc(mem, a, 128u);
    assert(in_arena(a));
    check_bytes(a, 128u, 0x42);
    dump(arena);
    assert(logged_blocks == 1u && logged_bytes == original_bytes / 2u);
    assert(h2_pal_mem_realloc(mem, b, 0u) == NULL);
    h2_pal_mem_free(mem, a);
    dump(arena);
    assert(logged_blocks == 0u && logged_bytes == 0u);
    finish(arena);
}

static void test_spill_short_backtrace(void) {
    h2_esp_platform_arena_t *arena = create(true);
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    caller_pc = 0x40003000u;
    for (unsigned scenario = 0u; scenario < 3u; ++scenario) {
        /* Reuse a formerly full-trace slot: short or corrupt walks must not
         * leave stale caller PCs in the remaining positions. */
        trace_frames = 6u;
        void *p = h2_pal_mem_alloc(mem, RESERVATION);
        assert(p != NULL);
        h2_pal_mem_free(mem, p);
        trace_frames = scenario == 0u ? 1u : 3u;
        corrupt_frame = scenario == 2u;
        const unsigned steps_before = frame_steps;
        p = h2_pal_mem_alloc(mem, RESERVATION);
        assert(p != NULL);
        dump(arena);
        assert(logged_blocks == 1u && logged_groups == 1u);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        const unsigned expected_frames = corrupt_frame ? 1u : trace_frames;
        for (unsigned i = 0u; i < 6u; ++i)
            assert(logged_frames[i] ==
                   (i < expected_frames ? caller_pc + i * 4u : 0u));
        assert(frame_steps - steps_before ==
               (corrupt_frame ? 1u : trace_frames - 1u));
#else
        assert(frame_steps == steps_before);
        for (unsigned i = 0u; i < 6u; ++i)
            assert(logged_frames[i] == 0u);
#endif
        h2_pal_mem_free(mem, p);
        corrupt_frame = false;
    }
    trace_frames = 6u;
    finish(arena);
}

static void test_census_site_probe(void) {
    uintptr_t caller = 1u, outer = 1u;
    assert(h2_esp_platform_arena_capture_site(NULL, NULL, &outer) ==
           H2_PAL_ERR_INVALID_ARG);
    caller_pc = 0x40005000u;
    trace_frames = 6u;
    capturing_census = true;
    const h2_pal_result_t rc = h2_esp_platform_arena_capture_site(
        NULL, &caller, &outer);
    capturing_census = false;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    assert(rc == H2_PAL_OK);
    assert(caller == 0x40005010u && outer == 0x40005014u);
#else
    assert(rc == H2_PAL_ERR_UNSUPPORTED);
    assert(caller == 0u && outer == 0u);
#endif
}

static void test_spill_capacity(void) {
    h2_esp_platform_arena_t *arena = create(true);
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    void *ptrs[513];
    for (unsigned i = 0u; i < 513u; ++i) {
        caller_pc = 0x40001000u + i * 32u;
        ptrs[i] = h2_pal_mem_alloc(mem, RESERVATION);
        assert(ptrs[i] != NULL);
    }
    dump(arena);
    assert(logged_blocks == 512u && logged_untracked == 1u);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    assert(logged_groups == 24u && logged_other > 0u);
#else
    assert(logged_groups == 1u && logged_other == 0u);
#endif
    h2_pal_mem_free(mem, ptrs[0]);
    ptrs[0] = h2_pal_mem_alloc(mem, RESERVATION);
    assert(ptrs[0] != NULL);
    dump(arena);
    assert(logged_blocks == 512u && logged_untracked == 1u);
    /* A formerly untracked block can enter a slot when resized after a free. */
    h2_pal_mem_free(mem, ptrs[1]);
    ptrs[1] = NULL;
    ptrs[512] = h2_pal_mem_realloc(mem, ptrs[512], 2u * RESERVATION);
    assert(ptrs[512] != NULL);
    dump(arena);
    assert(logged_blocks == 512u && logged_untracked == 1u);
    for (unsigned i = 0u; i < 513u; ++i)
        h2_pal_mem_free(mem, ptrs[i]);
    dump(arena);
    assert(logged_blocks == 0u && logged_bytes == 0u && logged_untracked == 1u);
    finish(arena);
}

static void test_spill_isolation_and_throttle(void) {
    h2_esp_platform_arena_t *a = create(true), *b = NULL;
    const h2_esp_platform_arena_config_t config = {
        .reserved_bytes = RESERVATION, .spill_to_system = true,
    };
    assert(h2_esp_platform_arena_create(&config, &b) == H2_PAL_OK);
    const h2_pal_mem_api_t *ma = h2_esp_platform_arena_mem(a);
    const h2_pal_mem_api_t *mb = h2_esp_platform_arena_mem(b);
    void *pa = h2_pal_mem_alloc(ma, RESERVATION);
    void *pb = h2_pal_mem_alloc(mb, 2u * RESERVATION);
    assert(pa != NULL && pb != NULL);
    unsigned before = summary_logs;
    now_us = 0;
    h2_esp_platform_arena_log_spills(a, "a", false);
    assert(summary_logs == before + 1u && logged_blocks == 1u);
    const size_t a_bytes = logged_bytes;
    h2_esp_platform_arena_log_spills(b, "b", false);
    assert(summary_logs == before + 2u && logged_blocks == 1u);
    assert(logged_bytes == a_bytes + RESERVATION);
    h2_esp_platform_arena_log_spills(a, "a", false);
    assert(summary_logs == before + 2u);
    now_us = 59999999;
    h2_esp_platform_arena_log_spills(a, "a", false);
    assert(summary_logs == before + 2u);
    now_us = 60000000;
    h2_esp_platform_arena_log_spills(a, "a", false);
    assert(summary_logs == before + 3u);
    dump(a);
    assert(summary_logs == before + 4u);
    before = failure_logs;
    fail_psram = true;
    for (unsigned i = 0u; i < 64u; ++i)
        assert(h2_pal_mem_alloc(ma, RESERVATION) == NULL);
    assert(failure_logs == before + 17u);
    assert(h2_pal_mem_alloc(mb, RESERVATION) == NULL);
    assert(failure_logs == before + 18u);
    fail_psram = false;
    h2_pal_mem_free(ma, pa);
    assert(h2_esp_platform_arena_destroy(a) == H2_PAL_OK);
    dump(b);
    assert(logged_blocks == 1u && logged_bytes == a_bytes + RESERVATION);
    h2_pal_mem_free(mb, pb);
    finish(b);
}

static void test_spill_registry_allocation_failure(void) {
    h2_esp_platform_arena_t *arena = NULL;
    h2_esp_platform_arena_config_t config = {
        .reserved_bytes = RESERVATION, .spill_to_system = true,
    };
    fail_registry = true;
    assert(h2_esp_platform_arena_create(&config, &arena) == H2_PAL_OK);
    fail_registry = false;
    assert(live_blocks == 2u);
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    void *p = h2_pal_mem_alloc(mem, RESERVATION);
    assert(p != NULL);
    dump(arena);
    assert(logged_blocks == 0u && logged_untracked == 1u);
    h2_pal_mem_free(mem, p);
    finish(arena);
    fail_mutex = true;
    assert(h2_esp_platform_arena_create(&config, &arena) == H2_PAL_ERR_NO_MEMORY);
    fail_mutex = false;
    assert(arena == NULL && live_blocks == 0u);
    config.small_pool_bytes = 1u;
    assert(h2_esp_platform_arena_create(&config, &arena) == H2_PAL_ERR_INVALID_ARG);
    assert(arena == NULL && live_blocks == 0u && mutex_count == 0u);
}

static void test_refusal_logging(void) {
    h2_esp_platform_arena_t *arena = create(false);
    const h2_pal_mem_api_t *mem = h2_esp_platform_arena_mem(arena);
    const unsigned before = refusal_logs;
    assert(h2_pal_mem_alloc(mem, RESERVATION) == NULL);
    assert(refusal_logs == before + 1u && last_refusal_count == 1u);
    assert(strstr(last_refusal, "op=alloc bytes=65536") != NULL);
    void *p = h2_pal_mem_alloc(mem, 128u);
    assert(p != NULL);
    assert(h2_pal_mem_realloc(mem, p, RESERVATION) == NULL);
    assert(refusal_logs == before + 2u && last_refusal_count == 2u);
    assert(strstr(last_refusal, "op=realloc bytes=65536") != NULL);
    assert(h2_pal_mem_realloc(mem, p, 0u) == NULL);
    assert(refusal_logs == before + 2u);
    for (unsigned i = 2u; i < 64u; ++i)
        assert(h2_pal_mem_alloc(mem, RESERVATION) == NULL);
    assert(refusal_logs == before + 25u && last_refusal_count == 64u);
    finish(arena);
    arena = create(false);
    assert(h2_pal_mem_alloc(h2_esp_platform_arena_mem(arena), RESERVATION) == NULL);
    assert(refusal_logs == before + 26u && last_refusal_count == 1u);
    finish(arena);
}
#endif

int main(void) {
    test_arena();
    test_in_place_growth();
    test_fallback_and_migration();
    test_split_pools();
    test_default_no_spill();
    test_failure();
#if defined(ESP_PLATFORM)
    test_spill_lifecycle();
    test_spill_short_backtrace();
    test_census_site_probe();
    test_spill_capacity();
    test_spill_isolation_and_throttle();
    test_spill_registry_allocation_failure();
    test_refusal_logging();
#endif
    return 0;
}
