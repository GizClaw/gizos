#include "h2_lvgl_platform.h"
#include "h2_desktop_platform.h"
#include "lvgl.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef union allocation_header {
    size_t bytes;
    max_align_t alignment;
} allocation_header_t;

typedef struct allocator_state {
    atomic_size_t calls;
    atomic_size_t reallocs;
    atomic_size_t live;
    atomic_size_t bytes;
    atomic_size_t fail_call;
} allocator_state_t;

static void *test_alloc(void *user, size_t bytes) {
    allocator_state_t *state = user;
    size_t call = atomic_fetch_add(&state->calls, 1u) + 1u;
    if (call == state->fail_call || bytes > SIZE_MAX - sizeof(allocation_header_t))
        return NULL;
    allocation_header_t *header = malloc(sizeof(*header) + bytes);
    if (header == NULL) return NULL;
    header->bytes = bytes;
    ++state->live;
    state->bytes += bytes;
    return header + 1;
}

static void test_free(void *user, void *ptr) {
    if (ptr == NULL) return;
    allocator_state_t *state = user;
    allocation_header_t *header = (allocation_header_t *)ptr - 1;
    assert(state->live > 0u);
    --state->live;
    state->bytes -= header->bytes;
    free(header);
}

static void *test_realloc(void *user, void *ptr, size_t bytes) {
    allocator_state_t *state = user;
    ++state->reallocs;
    if (bytes == 0u) {
        test_free(user, ptr);
        return NULL;
    }
    /* Always move successful reallocs so direct-list repairs are exercised. */
    void *resized = test_alloc(user, bytes);
    if (resized != NULL && ptr != NULL) {
        size_t old = ((allocation_header_t *)ptr - 1)->bytes;
        memcpy(resized, ptr, old < bytes ? old : bytes);
        test_free(user, ptr);
    }
    return resized;
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static h2_lvgl_platform_config_t config_for(const h2_pal_mem_api_t *mem) {
    return (h2_lvgl_platform_config_t){
        .allocator = mem,
        .task_api = h2_desktop_platform_task_api(),
        .sync_api = h2_desktop_platform_sync_api(),
        .queue_api = h2_desktop_platform_queue_api(),
        .time_api = h2_desktop_platform_time_api(),
        .pool_initial_bytes = 1024u,
        .pool_grow_bytes = 2048u,
    };
}

static h2_lvgl_memory_stats_t snapshot(void) {
    h2_lvgl_memory_stats_t stats;
    assert(h2_lvgl_platform_get_memory_stats(&stats) == H2_PAL_OK);
    return stats;
}

static void assert_empty(allocator_state_t *state) {
    assert(state->live == 0u);
    assert(state->bytes == 0u);
    h2_lvgl_memory_stats_t stats = snapshot();
    assert(stats.chunks == 0u && stats.chunk_bytes == 0u);
    assert(stats.used_bytes == 0u && stats.largest_free_bytes == 0u);
    assert(stats.control_bytes == 0u && stats.direct_blocks == 0u);
    assert(stats.direct_bytes == 0u);
}

static void check_bytes(const void *ptr, size_t size, unsigned char value) {
    assert(ptr != NULL);
    assert((uintptr_t)ptr % _Alignof(max_align_t) == 0u);
    for (size_t i = 0u; i < size; ++i)
        assert(((const unsigned char *)ptr)[i] == value);
}

static void test_growth_and_realloc(void) {
    allocator_state_t state = {0};
    const h2_pal_mem_api_t mem = {&state, &s_mem_vtable};
    h2_lvgl_platform_config_t config = config_for(&mem);
    assert(h2_lvgl_platform_init(&config) == 0);
    h2_lvgl_memory_stats_t before = snapshot();
    assert(before.chunks == 1u && before.used_bytes == 0u);
    const size_t descriptor = before.chunk_bytes - config.pool_initial_bytes;
    const size_t initial_calls = state.calls;
    void *blocks[2000];
    for (size_t i = 0u; i < 2000u; ++i) {
        blocks[i] = lv_malloc_core(16u);
        assert(blocks[i] != NULL);
        memset(blocks[i], (unsigned char)i, 16u);
        check_bytes(blocks[i], 16u, (unsigned char)i);
    }
    h2_lvgl_memory_stats_t filled = snapshot();
    assert(filled.chunks > 1u && filled.chunks < 100u);
    assert(state.calls - initial_calls == filled.chunks - 1u);
    assert(filled.chunk_bytes == before.chunk_bytes +
        (filled.chunks - 1u) * (config.pool_grow_bytes + descriptor));
    assert(filled.used_bytes >= 32000u && filled.used_bytes < 64000u);
    for (size_t i = 0u; i < 2000u; ++i) {
        check_bytes(blocks[i], 16u, (unsigned char)i);
        lv_free_core(blocks[i]);
    }
    h2_lvgl_memory_stats_t freed = snapshot();
    assert(freed.used_bytes == 0u && freed.chunks == filled.chunks);
    assert(freed.largest_free_bytes > filled.largest_free_bytes);

    unsigned char *ptr = lv_realloc_core(NULL, 16u);
    assert(ptr != NULL);
    memset(ptr, 0x6a, 16u);
    ptr = lv_realloc_core(ptr, 512u);
    check_bytes(ptr, 16u, 0x6a);
    memset(ptr, 0x7b, 512u);
    ptr = lv_realloc_core(ptr, 8192u);
    check_bytes(ptr, 512u, 0x7b);
    assert(snapshot().direct_blocks == 1u);
    void *other = lv_malloc_core(4096u);
    assert(other != NULL);
    ptr = lv_realloc_core(ptr, 16384u); /* Move a non-head direct block. */
    check_bytes(ptr, 512u, 0x7b);
    assert(snapshot().direct_bytes == 20480u);
    ptr = lv_realloc_core(ptr, 32u); /* Remains direct after shrinking. */
    check_bytes(ptr, 32u, 0x7b);
    assert(snapshot().direct_bytes == 4128u);
    assert(lv_realloc_core(ptr, 0u) == NULL);
    lv_free_core(other);
    assert(snapshot().direct_blocks == 0u);
    ptr = lv_malloc_core(128u);
    assert(ptr != NULL);
    memset(ptr, 0x3c, 128u);
    ptr = lv_realloc_core(ptr, 8u);
    check_bytes(ptr, 8u, 0x3c);
    lv_free_core(ptr);
    assert(lv_malloc_core(0u) == NULL);
    lv_free_core(NULL);
    assert(lv_malloc_core(SIZE_MAX) == NULL);
    h2_lvgl_platform_deinit();
    assert_empty(&state);
}

static void test_isolated_block_reuse(void) {
    allocator_state_t state = {0};
    const h2_pal_mem_api_t mem = {&state, &s_mem_vtable};
    h2_lvgl_platform_config_t config = config_for(&mem);
    config.pool_initial_bytes = 512u;
    config.pool_grow_bytes = 512u;
    assert(h2_lvgl_platform_init(&config) == 0);
    state.fail_call = state.calls + 1u;
    void *blocks[64];
    size_t count = 0u;
    while (count < 64u) {
        blocks[count] = lv_malloc_core(16u);
        if (blocks[count] == NULL) break;
        ++count;
    }
    assert(count > 2u && count < 64u);
    const size_t middle = count / 2u;
    lv_free_core(blocks[middle]);
    state.fail_call = state.calls + 1u;
    const size_t calls = state.calls;
    blocks[middle] = lv_malloc_core(16u);
    assert(blocks[middle] != NULL);
    assert(state.calls == calls); /* An isolated freed block must suffice. */
    assert((uintptr_t)blocks[middle] % _Alignof(max_align_t) == 0u);
    for (size_t i = 0u; i < count; ++i) lv_free_core(blocks[i]);
    h2_lvgl_platform_deinit();
    assert_empty(&state);
}

static void test_failures(void) {
    allocator_state_t state = {0};
    const h2_pal_mem_api_t mem = {&state, &s_mem_vtable};
    h2_lvgl_platform_config_t config = config_for(&mem);
    config.pool_initial_bytes = 255u;
    assert(h2_lvgl_platform_init(&config) < 0);
    config.pool_initial_bytes = SIZE_MAX;
    assert(h2_lvgl_platform_init(&config) < 0);
    config.pool_initial_bytes = 1024u;
    config.pool_grow_bytes = 1u;
    assert(h2_lvgl_platform_init(&config) < 0);
    config.pool_grow_bytes = SIZE_MAX;
    assert(h2_lvgl_platform_init(&config) < 0);
    assert_empty(&state);
    config = config_for(&mem);
    const h2_pal_sync_api_t no_sync = {0};
    config.sync_api = &no_sync;
    assert(h2_lvgl_platform_init(&config) == H2_PAL_ERR_UNSUPPORTED);
    assert_empty(&state);
    config = config_for(&mem);
    h2_pal_mem_vtable_t incomplete_vtable = s_mem_vtable;
    incomplete_vtable.realloc = NULL;
    const h2_pal_mem_api_t incomplete_mem = {&state, &incomplete_vtable};
    config.allocator = &incomplete_mem;
    assert(h2_lvgl_platform_init(&config) == H2_PAL_ERR_INVALID_ARG);
    assert_empty(&state);
    config = config_for(&mem);
    /* Fail each backing acquisition, including the PAL mutex. */
    unsigned failures = 0u;
    for (size_t failure = 1u; failure < 16u; ++failure) {
        state.calls = 0u;
        state.fail_call = failure;
        int rc = h2_lvgl_platform_init(&config);
        h2_lvgl_platform_deinit();
        assert_empty(&state);
        if (rc == 0) break;
        ++failures;
    }
    assert(failures >= 3u);
    state.fail_call = 0u;
    assert(h2_lvgl_platform_init(&config) == 0);
    assert(h2_lvgl_platform_init(&config) < 0);
    unsigned char *ptr = lv_malloc_core(16u);
    assert(ptr != NULL);
    memset(ptr, 0x25, 16u);
    size_t chunks = snapshot().chunks;
    state.fail_call = state.calls + 1u;
    assert(lv_realloc_core(ptr, 1024u) == NULL); /* Needs a growth chunk. */
    check_bytes(ptr, 16u, 0x25);
    assert(snapshot().chunks == chunks);
    state.fail_call = state.calls + 1u;
    assert(lv_realloc_core(ptr, 8192u) == NULL);
    check_bytes(ptr, 16u, 0x25);
    state.fail_call = 0u;
    ptr = lv_realloc_core(ptr, 1024u);
    check_bytes(ptr, 16u, 0x25);
    assert(snapshot().chunks == chunks + 1u);
    ptr = lv_realloc_core(ptr, 8192u);
    check_bytes(ptr, 16u, 0x25);
    state.fail_call = state.calls + 1u;
    assert(lv_realloc_core(ptr, 16384u) == NULL);
    check_bytes(ptr, 16u, 0x25);
    assert(snapshot().direct_bytes == 8192u);
    assert(lv_realloc_core(ptr, SIZE_MAX) == NULL);
    check_bytes(ptr, 16u, 0x25);
    lv_free_core(ptr);
    state.fail_call = 0u;
    h2_lvgl_platform_deinit();
    assert_empty(&state);
}

static void allocation_worker(void *ctx) {
    const unsigned char value = *(const unsigned char *)ctx;
    for (size_t i = 0u; i < 1000u; ++i) {
        size_t size = 16u + i % 112u;
        void *ptr = lv_malloc_core(size);
        assert(ptr != NULL);
        memset(ptr, value, size);
        ptr = lv_realloc_core(ptr, i % 7u == 0u ? 4096u : 256u);
        check_bytes(ptr, size, value);
        lv_free_core(ptr);
        (void)snapshot();
    }
}

static void test_concurrent_hooks(void) {
    allocator_state_t state = {0};
    const h2_pal_mem_api_t mem = {&state, &s_mem_vtable};
    h2_lvgl_platform_config_t config = config_for(&mem);
    assert(h2_lvgl_platform_init(&config) == 0);
    h2_pal_task_t *tasks[4] = {0};
    unsigned char values[4] = {1u, 2u, 3u, 4u};
    const h2_pal_task_options_t options = {.name = "pool-test"};
    for (size_t i = 0u; i < 4u; ++i)
        assert(h2_pal_task_start(config.task_api, &options,
            allocation_worker, &values[i], &tasks[i]) == H2_PAL_OK);
    for (size_t i = 0u; i < 4u; ++i)
        assert(h2_pal_task_join(config.task_api, tasks[i]) == H2_PAL_OK);
    assert(snapshot().used_bytes == 0u);
    assert(snapshot().direct_blocks == 0u);
    h2_lvgl_platform_deinit();
    assert_empty(&state);
}

static void test_lifecycle(void) {
    allocator_state_t state = {0};
    const h2_pal_mem_api_t mem = {&state, &s_mem_vtable};
    h2_lvgl_platform_config_t config = config_for(&mem);
    config.pool_grow_bytes = 0u;
    for (size_t binding = 0u; binding < 3u; ++binding) {
        assert(h2_lvgl_platform_init(&config) == 0);
        for (size_t cycle = 0u; cycle < 2u; ++cycle) {
            lv_init();
            lv_lock();
            lv_unlock();
            assert(lv_malloc_core(16u) != NULL);
            assert(lv_malloc_core(8192u) != NULL);
            assert(snapshot().direct_blocks > 0u);
            lv_deinit(); /* Also releases the deliberately outstanding blocks. */
            assert_empty(&state);
            assert(lv_malloc_core(1u) == NULL);
        }
        h2_lvgl_platform_deinit();
        h2_lvgl_platform_deinit();
        assert_empty(&state);
    }
    config.pool_initial_bytes = 0u;
    config.pool_grow_bytes = SIZE_MAX; /* Ignored in default mode. */
    state.calls = 0u;
    state.reallocs = 0u;
    assert(h2_lvgl_platform_init(&config) == 0);
    assert(state.calls == 0u);
    void *ptr = lv_malloc_core(16u);
    assert(state.calls == 1u && state.live == 1u);
    ptr = lv_realloc_core(ptr, 32u);
    assert(ptr != NULL && state.reallocs == 1u);
    assert(snapshot().chunks == 0u && snapshot().direct_blocks == 0u);
    lv_free_core(ptr);
    h2_lvgl_platform_deinit();
    assert_empty(&state);
    assert(h2_lvgl_platform_get_memory_stats(NULL) == H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
    test_growth_and_realloc();
    test_isolated_block_reuse();
    test_failures();
    test_concurrent_hooks();
    test_lifecycle();
    return 0;
}
