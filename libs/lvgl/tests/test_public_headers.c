#include "h2_lvgl_osal.h"
#include "h2_lvgl_platform.h"
#include "h2_lvgl_task_names.h"
#include "h2_lvgl_touch.h"
#include "h2_lvgl_button.h"
#include "h2_lvgl_fs.h"
#include "h2_lvgl_fs_internal.h"
#include "lvgl.h"
#include "osal/lv_os_private.h"

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct allocator_test_state {
    unsigned int alloc_calls;
    unsigned int realloc_calls;
    unsigned int free_calls;
} allocator_test_state_t;

typedef struct task_test_state {
    const char *last_name;
    unsigned int start_calls;
} task_test_state_t;

static int test_task_start(void *user, const h2_pal_task_options_t *options,
                           h2_pal_task_entry_t entry, void *ctx,
                           h2_pal_task_t **out_task) {
    task_test_state_t *state = user;
    (void)entry;
    (void)ctx;
    state->last_name = options->name;
    state->start_calls++;
    *out_task = (h2_pal_task_t *)state;
    return H2_PAL_OK;
}

static int test_task_join(void *user, h2_pal_task_t *task) {
    (void)user;
    (void)task;
    return H2_PAL_OK;
}

static void test_thread_entry(void *user) {
    (void)user;
}

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
    static const h2_pal_task_vtable_t task_vtable = {
        .start = test_task_start,
        .join = test_task_join,
    };
    static const h2_pal_sync_api_t sync_api = {0};
    static const h2_pal_queue_api_t queue_api = {0};
    static const h2_pal_time_api_t time_api = {0};
    allocator_test_state_t state = {0};
    task_test_state_t task_state = {0};
    const h2_pal_mem_api_t mem_api = {
        .user = &state,
        .vtable = &mem_vtable,
    };
    const h2_pal_task_api_t task_api = {
        .user = &task_state,
        .vtable = &task_vtable,
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

    static const struct {
        const char *upstream_name;
        const char *portable_name;
    } task_names[] = {
        {"swdraw", h2_lvgl_software_draw_task_name},
        {"g2ddraw", h2_lvgl_g2d_draw_task_name},
        {"pxpdraw", h2_lvgl_pxp_draw_task_name},
        {"nemagfx", h2_lvgl_nema_gfx_task_name},
    };
    for (size_t i = 0u; i < sizeof(task_names) / sizeof(task_names[0]); ++i) {
        lv_thread_t thread = NULL;
        assert(lv_thread_init(&thread, task_names[i].upstream_name,
                              LV_THREAD_PRIO_MID, test_thread_entry, 4096u,
                              NULL) == LV_RESULT_OK);
        assert(task_state.last_name == task_names[i].portable_name);
        assert(lv_thread_delete(&thread) == LV_RESULT_OK);
    }
    lv_thread_t unknown_thread = NULL;
    assert(lv_thread_init(&unknown_thread, "unknown", LV_THREAD_PRIO_MID,
                          test_thread_entry, 4096u,
                          NULL) == LV_RESULT_INVALID);
    assert(task_state.start_calls == sizeof(task_names) / sizeof(task_names[0]));

    h2_lvgl_platform_deinit();
    assert(lv_malloc_core(1u) == NULL);
}

int main(void) {
    test_relative_seek_target();
    test_pal_allocator_bridge();
    return 0;
}
