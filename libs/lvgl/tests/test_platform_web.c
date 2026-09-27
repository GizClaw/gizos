#include "h2_lvgl_platform.h"
#include "lvgl.h"

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct allocator_test_state {
    unsigned int alloc_calls;
    unsigned int realloc_calls;
    unsigned int free_calls;
    unsigned int live_blocks;
    int fail_alloc;
} allocator_test_state_t;

static void *test_alloc(void *user, size_t len) {
    allocator_test_state_t *state = user;
    state->alloc_calls++;
    if (state->fail_alloc) return NULL;
    void *ptr = malloc(len);
    if (ptr != NULL) ++state->live_blocks;
    return ptr;
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    allocator_test_state_t *state = user;
    state->realloc_calls++;
    const int was_null = ptr == NULL;
    void *resized = realloc(ptr, len);
    if (resized != NULL && was_null) ++state->live_blocks;
    return resized;
}

static void test_free(void *user, void *ptr) {
    allocator_test_state_t *state = user;
    state->free_calls++;
    if (ptr != NULL) {
        assert(state->live_blocks > 0u);
        --state->live_blocks;
    }
    free(ptr);
}

int main(void) {
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
    };
    static const h2_pal_task_api_t task_api = {0};
    static const h2_pal_sync_api_t sync_api = {0};
    static const h2_pal_queue_api_t queue_api = {0};
    static const h2_pal_time_api_t time_api = {0};
    allocator_test_state_t state = {0};
    const h2_pal_mem_api_t mem_api = {
        .user = &state,
        .vtable = &mem_vtable,
    };
    const h2_lvgl_platform_config_t config = {
        .allocator = &mem_api,
        .task_api = &task_api,
        .sync_api = &sync_api,
        .queue_api = &queue_api,
        .time_api = &time_api,
    };

    assert(h2_lvgl_platform_init(&config) == 0);
    void *memory = lv_malloc_core(16u);
    assert(memory != NULL);
    assert(state.alloc_calls == 1u);
    memory = lv_realloc_core(memory, 32u);
    assert(memory != NULL);
    assert(state.realloc_calls == 1u);
    lv_free_core(memory);
    assert(state.free_calls == 1u);

    memory = lv_realloc_core(NULL, 8u);
    assert(memory != NULL);
    assert(state.alloc_calls == 2u);
    assert(state.realloc_calls == 1u);
    lv_free_core(memory);
    assert(state.free_calls == 2u);
    assert(lv_mem_test_core() == LV_RESULT_OK);
    assert(state.alloc_calls == 3u);
    assert(state.free_calls == 3u);

    h2_lvgl_platform_deinit();
    assert(lv_malloc_core(1u) == NULL);
    assert(state.live_blocks == 0u);
    h2_lvgl_platform_config_t pooled = config;
    pooled.pool_initial_bytes = 1024u;
    pooled.pool_grow_bytes = 2048u;
    for (unsigned cycle = 0u; cycle < 3u; ++cycle) {
        assert(h2_lvgl_platform_init(&pooled) == 0);
        lv_init();
        for (unsigned block = 0u; block < 300u; ++block) {
            void *small = lv_malloc_core(16u);
            assert(small != NULL);
            assert((uintptr_t)small % _Alignof(max_align_t) == 0u);
        }
        void *large = lv_malloc_core(8192u);
        assert(large != NULL);
        assert((uintptr_t)large % _Alignof(max_align_t) == 0u);
        memset(large, 0x5a, 8192u);
        large = lv_realloc_core(large, 16384u);
        assert(large != NULL);
        assert((uintptr_t)large % _Alignof(max_align_t) == 0u);
        for (size_t byte = 0u; byte < 8192u; ++byte)
            assert(((unsigned char *)large)[byte] == 0x5a);
        h2_lvgl_memory_stats_t stats;
        assert(h2_lvgl_platform_get_memory_stats(&stats) == 0);
        assert(stats.chunks > 1u && stats.direct_blocks > 0u);
        lv_deinit();
        assert(state.live_blocks == 0u);
        assert(h2_lvgl_platform_get_memory_stats(&stats) == 0);
        assert(stats.chunks == 0u && stats.direct_blocks == 0u);
        h2_lvgl_platform_deinit();
    }
    pooled.pool_initial_bytes = 512u;
    pooled.pool_grow_bytes = 512u;
    assert(h2_lvgl_platform_init(&pooled) == 0);
    state.fail_alloc = 1;
    void *blocks[64];
    size_t count = 0u;
    while (count < 64u) {
        blocks[count] = lv_malloc_core(16u);
        if (blocks[count] == NULL) break;
        ++count;
    }
    assert(count > 2u && count < 64u);
    lv_free_core(blocks[count / 2u]);
    const unsigned calls = state.alloc_calls;
    blocks[count / 2u] = lv_malloc_core(16u);
    assert(blocks[count / 2u] != NULL);
    assert((uintptr_t)blocks[count / 2u] % _Alignof(max_align_t) == 0u);
    assert(state.alloc_calls == calls);
    h2_lvgl_platform_deinit();
    assert(state.live_blocks == 0u);
    return 0;
}
