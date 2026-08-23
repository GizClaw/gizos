#include "h2_lvgl_osal.h"
#include "h2_lvgl_platform.h"
#include "h2_lvgl_touch.h"
#include "h2_lvgl_button.h"
#include "h2_lvgl_fs.h"
#include "h2_lvgl_fs_internal.h"
#include "lvgl.h"

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct allocator_test_state {
    unsigned int alloc_calls;
    unsigned int realloc_calls;
    unsigned int free_calls;
} allocator_test_state_t;

static void *test_alloc(void *user, size_t len) {
    allocator_test_state_t *state = user;
    state->alloc_calls++;
    return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    allocator_test_state_t *state = user;
    state->realloc_calls++;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    allocator_test_state_t *state = user;
    state->free_calls++;
    free(ptr);
}

static void test_relative_seek_target(void) {
    uint32_t target = 0u;

    assert(h2_lvgl_fs_relative_target(25u, (uint32_t)(int32_t)-3,
                                      &target));
    assert(target == 22u);
    assert(h2_lvgl_fs_relative_target(100u, 5u, &target));
    assert(target == 105u);
    assert(!h2_lvgl_fs_relative_target(0u, (uint32_t)(int32_t)-1,
                                       &target));
    assert(!h2_lvgl_fs_relative_target(UINT32_MAX, 1u, &target));
    assert(!h2_lvgl_fs_relative_target(0u, 0u, NULL));
}

static void test_pal_allocator_bridge(void) {
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
}

int main(void) {
    test_relative_seek_target();
    test_pal_allocator_bridge();
    return 0;
}
