#include "h2_lvgl_display.h"
#include "h2_lvgl_platform.h"
#include "h2_desktop_platform.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

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

int main(void) {
    test_state_t state = {.width = 64, .height = 48};
    const h2_pal_mem_api_t *mem = h2_desktop_platform_default_allocator();
    const h2_lvgl_platform_config_t platform = {
        mem,
        h2_desktop_platform_task_api(),
        h2_desktop_platform_sync_api(),
        h2_desktop_platform_queue_api(),
        h2_desktop_platform_time_api(),
    };
    const h2_pal_display_vtable_t display_vtable = {
        display_open, display_info, display_draw, display_present, NULL,
        display_close};
    h2_pal_display_t display = {&state, &display_vtable};

    assert(h2_lvgl_platform_init(&platform) == 0);
    lv_init();
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
    return 0;
}
