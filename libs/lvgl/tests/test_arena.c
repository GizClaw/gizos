#include "h2_lvgl_display.h"
#include "h2_lvgl_platform.h"
#include "h2_desktop_platform.h"
#include "h2_mem_arena.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_state {
    int width;
    int height;
    size_t open_calls;
    size_t close_calls;
    size_t draw_calls;
    size_t present_calls;
    size_t last_stride;
    h2_display_rect_t last_rect;
} test_state_t;

typedef struct allocator_state {
    size_t calls;
    size_t fail_call;
    size_t frees;
} allocator_state_t;

static void *test_alloc(void *user, size_t len) {
    allocator_state_t *state = user;
    ++state->calls;
    return state->calls == state->fail_call ? NULL : malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    allocator_state_t *state = user;
    ++state->frees;
    free(ptr);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static int display_open(void *user) {
    ((test_state_t *)user)->open_calls++;
    return H2_DISPLAY_OK;
}

static int display_info(void *user, h2_display_info_t *info) {
    test_state_t *state = user;
    *info = (h2_display_info_t){state->width, state->height,
                               H2_DISPLAY_PIXEL_RGB565};
    return H2_DISPLAY_OK;
}

static int display_draw(void *user, const h2_display_rect_t *rect,
                        const void *pixels, size_t stride,
                        h2_display_pixel_format_t format) {
    test_state_t *state = user;
    assert(pixels != NULL);
    assert(format == H2_DISPLAY_PIXEL_RGB565);
    state->draw_calls++;
    state->last_rect = *rect;
    state->last_stride = stride;
    return H2_DISPLAY_OK;
}

static int display_present(void *user) {
    ((test_state_t *)user)->present_calls++;
    return H2_DISPLAY_OK;
}

static int display_close(void *user) {
    ((test_state_t *)user)->close_calls++;
    return H2_DISPLAY_OK;
}

static void arena_lock(void *user) {
    assert(h2_pal_mutex_lock(h2_desktop_platform_sync_api(), user) == H2_PAL_OK);
}

static void arena_unlock(void *user) {
    assert(h2_pal_mutex_unlock(h2_desktop_platform_sync_api(), user) == H2_PAL_OK);
}

static h2_mem_arena_stats_t arena_stats(h2_mem_arena_t *arena) {
    h2_mem_arena_stats_t stats;
    assert(h2_mem_arena_stats(arena, &stats) == H2_PAL_OK);
    return stats;
}

int main(void) {
    test_state_t state = {.width = 64, .height = 48};
    const h2_pal_mem_api_t *fallback = h2_desktop_platform_default_allocator();
    const h2_pal_mutex_config_t mutex_config = {.allocator = fallback};
    h2_pal_mutex_t *mutex = NULL;
    assert(h2_pal_mutex_create(h2_desktop_platform_sync_api(), &mutex_config,
                              &mutex) == H2_PAL_OK);
    const size_t block_bytes = 1024u * 1024u;
    void *block = malloc(block_bytes);
    assert(block != NULL);
    const h2_mem_arena_config_t arena_config = {
        .block = block, .block_bytes = block_bytes,
        .small_request_max = 32u * 1024u, .small_pool_bytes = 128u * 1024u,
        .fallback = fallback, .lock = arena_lock, .unlock = arena_unlock,
        .lock_user = mutex,
    };
    h2_mem_arena_t *arena = NULL;
    assert(h2_mem_arena_create(&arena_config, &arena) == H2_PAL_OK);
    const h2_pal_mem_api_t *mem = h2_mem_arena_mem(arena);
    const h2_lvgl_platform_config_t platform = {
        mem,
        h2_desktop_platform_task_api(),
        h2_desktop_platform_sync_api(),
        h2_desktop_platform_queue_api(),
        h2_desktop_platform_time_api(),
        0u, 0u,
    };
    const h2_pal_display_vtable_t display_vtable = {
        display_open, display_info, display_draw, display_present, NULL,
        display_close};
    h2_pal_display_t display = {&state, &display_vtable};

    assert(h2_lvgl_platform_init(&platform) == 0);
    lv_init();
    const h2_mem_arena_stats_t initialized = arena_stats(arena);
    assert(initialized.small.live_bytes > 0u);
    unsigned char *payload = lv_malloc(32u);
    assert(payload != NULL);
    memset(payload, 0xa5, 32u);
    assert(arena_stats(arena).small.live_bytes > initialized.small.live_bytes);
    payload = lv_realloc(payload, 64u * 1024u);
    assert(payload != NULL);
    assert(arena_stats(arena).large.live_bytes > initialized.large.live_bytes);
    payload = lv_realloc(payload, block_bytes);
    assert(payload != NULL);
    assert(arena_stats(arena).large.fallback_live_bytes >= block_bytes);
    for (size_t i = 0u; i < 32u; ++i)
        assert(payload[i] == 0xa5);
    lv_free(payload);
    assert(arena_stats(arena).fallback_live_bytes == initialized.fallback_live_bytes);
    assert(h2_mem_arena_destroy(arena) == H2_PAL_ERR_INVALID_STATE);
    h2_lvgl_display_t *adapter = NULL;
    const h2_lvgl_display_config_t config = {&display, mem, 8u};
    assert(h2_lvgl_display_create(&config, &adapter) == H2_PAL_OK);
    assert(adapter != NULL);
    assert(state.open_calls == 1u);
    lv_obj_t *screen = lv_obj_create(NULL);
    assert(screen != NULL);
    lv_screen_load(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x123456), LV_PART_MAIN);
    lv_obj_invalidate(screen);
    lv_refr_now(h2_lvgl_display_lvgl(adapter));
    assert(state.draw_calls > 0u);
    assert(state.present_calls > 0u);
    assert(state.last_rect.width > 0 && state.last_rect.height > 0);
    assert(state.last_stride >= (size_t)state.last_rect.width * 2u);
    assert(h2_lvgl_display_last_result(adapter) == H2_PAL_OK);
    h2_lvgl_display_destroy(adapter);
    assert(state.close_calls == 1u);

    state.width = (INT_MAX / 2) + 1;
    state.height = 2;
    adapter = NULL;
    assert(h2_lvgl_display_create(&config, &adapter) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(adapter == NULL);
    assert(state.open_calls == 2u);
    assert(state.close_calls == 2u);

    state.width = 64;
    state.height = 48;
    allocator_state_t allocator = {.fail_call = 1u};
    const h2_pal_mem_api_t test_mem = {&allocator, &test_mem_vtable};
    const h2_lvgl_display_config_t failure_config = {&display, &test_mem, 8u};
    assert(h2_lvgl_display_create(&failure_config, &adapter) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(adapter == NULL);
    assert(allocator.frees == 0u);
    assert(state.close_calls == 3u);

    allocator.calls = 0u;
    allocator.fail_call = 2u;
    assert(h2_lvgl_display_create(&failure_config, &adapter) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(adapter == NULL);
    assert(allocator.frees == 1u);
    assert(state.close_calls == 4u);
    lv_deinit();
    h2_lvgl_platform_deinit();
    const h2_mem_arena_stats_t final = arena_stats(arena);
    assert(final.small.live_bytes == 0u && final.large.live_bytes == 0u);
    assert(final.fallback_live_bytes == 0u);
    // Rebinding the same allocator must not accumulate a general OS mutex.
    for (unsigned cycle = 0u; cycle < 2u; ++cycle) {
        assert(h2_lvgl_platform_init(&platform) == 0);
        lv_init();
        assert(arena_stats(arena).small.live_bytes > 0u);
        lv_deinit();
        h2_lvgl_platform_deinit();
        const h2_mem_arena_stats_t released = arena_stats(arena);
        assert(released.small.live_bytes == 0u && released.large.live_bytes == 0u);
        assert(released.fallback_live_bytes == 0u);
    }
    assert(h2_mem_arena_destroy(arena) == H2_PAL_OK);
    free(block);
    assert(h2_pal_mutex_destroy(h2_desktop_platform_sync_api(), mutex) == H2_PAL_OK);
    return 0;
}
