#include "h2_display_viewport.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct backend {
    uint16_t frame[800u * 480u];
    int opens, closes, presents, draws;
    int open_rc, info_rc, close_rc, draw_rc, present_rc;
    uint32_t brightness;
    h2_display_pixel_format_t format;
    size_t stride;
    const void *pixels;
    h2_display_rect_t last;
} backend_t;

static void *allocate(void *user, size_t size) { (void)user; return malloc(size); }
static void release(void *user, void *ptr) { (void)user; free(ptr); }
static void *no_memory(void *user, size_t size) { (void)user; (void)size; return NULL; }
static const h2_pal_mem_vtable_t memory_ops = {.alloc = allocate, .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_ops};

static int open_display(void *user) {
    backend_t *b = user; b->opens++; return b->open_rc;
}
static int info_display(void *user, h2_display_info_t *info) {
    backend_t *b = user;
    *info = (h2_display_info_t){800, 480, H2_DISPLAY_PIXEL_RGB565};
    return b->info_rc;
}
static int close_display(void *user) {
    backend_t *b = user; b->closes++; return b->close_rc;
}
static int draw_display(void *user, const h2_display_rect_t *rect,
                        const void *pixels, size_t stride,
                        h2_display_pixel_format_t format) {
    backend_t *b = user;
    b->draws++; b->last = *rect; b->pixels = pixels; b->stride = stride; b->format = format;
    if (b->draw_rc != 0) return b->draw_rc;
    if (format == H2_DISPLAY_PIXEL_RGB565) {
        for (int y = 0; y < rect->height; y++) {
            memcpy(&b->frame[(rect->y + y) * 800 + rect->x],
                   (const uint8_t *)pixels + (size_t)y * stride,
                   (size_t)rect->width * 2u);
        }
    }
    return 0;
}
static int present_display(void *user) {
    backend_t *b = user; b->presents++; return b->present_rc;
}
static int brightness_display(void *user, uint32_t value) {
    backend_t *b = user; b->brightness = value; return 0;
}
static const h2_pal_display_vtable_t operations = {
    .open = open_display, .get_info = info_display, .close = close_display,
    .draw_bitmap = draw_display, .present = present_display,
    .set_brightness_percent = brightness_display,
};

int main(void) {
    static backend_t b;
    const h2_pal_display_api_t backend = {.user = &b, .vtable = &operations};
    h2_display_rect_t area = {280, 120, 240, 240};
    h2_display_viewport_t *v = NULL;
    assert(h2_display_viewport_create(&memory, &backend, &area, &v) == H2_PAL_OK);
    const h2_pal_display_api_t *api = h2_display_viewport_api(v);
    h2_display_info_t info = {1, 1, H2_DISPLAY_PIXEL_RGB888};
    assert(h2_pal_display_get_info(api, &info) == H2_DISPLAY_ERR_INVALID_STATE);
    assert(info.width == 0);
    assert(h2_pal_display_open(api) == 0);
    assert(h2_pal_display_open(api) == 0 && b.opens == 1);
    assert(h2_pal_display_get_info(api, &info) == 0);
    assert(info.width == 240 && info.height == 240 && info.native_format == H2_DISPLAY_PIXEL_RGB565);
    uint16_t pixels[] = {0xf800, 0x07e0, 0xffff, 0x001f, 0xffff, 0xffff};
    h2_display_rect_t rect = {0, 0, 2, 2};
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 6, H2_DISPLAY_PIXEL_RGB565) == 0);
    assert(b.last.x == 280 && b.last.y == 120 && b.pixels == pixels);
    assert(b.frame[120 * 800 + 280] == 0xf800);
    assert(b.frame[121 * 800 + 280] == 0x001f);
    assert(b.frame[120 * 800 + 279] == 0 && b.frame[119 * 800 + 280] == 0);
    rect = (h2_display_rect_t){239, 239, 1, 1};
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 2, H2_DISPLAY_PIXEL_RGB565) == 0);
    assert(b.frame[359 * 800 + 519] == 0xf800);
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 3, H2_DISPLAY_PIXEL_RGB888) == 0);
    assert(b.format == H2_DISPLAY_PIXEL_RGB888 && b.stride == 3);
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 2, H2_DISPLAY_PIXEL_RGB444) == 0);
    int draws = b.draws;
    rect.x = 240;
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 2, H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_ERR_INVALID_ARG);
    rect.x = -1;
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 2, H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_ERR_INVALID_ARG);
    rect = (h2_display_rect_t){0, 0, 2, 3};
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 3, H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_ERR_INVALID_ARG);
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, SIZE_MAX, H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_ERR_INVALID_ARG);
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 6, (h2_display_pixel_format_t)99) == H2_DISPLAY_ERR_UNSUPPORTED);
    assert(b.draws == draws);
    b.draw_rc = H2_DISPLAY_ERR_IO;
    assert(h2_pal_display_draw_bitmap(api, &rect, pixels, 6, H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_ERR_IO);
    b.present_rc = H2_DISPLAY_ERR_IO;
    assert(h2_pal_display_present(api) == H2_DISPLAY_ERR_IO && b.presents == 1);
    assert(h2_pal_display_set_brightness_percent(api, 50) == 0 && b.brightness == 50);
    assert(h2_pal_display_set_brightness_percent(api, 101) == H2_DISPLAY_ERR_INVALID_ARG && b.brightness == 50);
    b.close_rc = H2_DISPLAY_ERR_IO;
    assert(h2_display_viewport_destroy(v) == H2_DISPLAY_ERR_IO);
    b.close_rc = 0;
    assert(h2_pal_display_close(api) == 0);
    assert(h2_pal_display_close(api) == 0 && b.closes == 2);
    assert(h2_pal_display_present(api) == H2_DISPLAY_ERR_INVALID_STATE);
    assert(h2_pal_display_open(api) == 0 && b.opens == 2);
    assert(h2_display_viewport_destroy(v) == 0);

    area = (h2_display_rect_t){799, 0, 2, 2};
    assert(h2_display_viewport_create(&memory, &backend, &area, &v) == 0);
    api = h2_display_viewport_api(v);
    b.close_rc = H2_DISPLAY_ERR_IO;
    assert(h2_pal_display_open(api) == H2_DISPLAY_ERR_IO);
    assert(h2_pal_display_open(api) == H2_DISPLAY_ERR_INVALID_STATE);
    assert(h2_display_viewport_destroy(v) == H2_DISPLAY_ERR_IO);
    b.close_rc = 0;
    assert(h2_display_viewport_destroy(v) == 0);
    area = (h2_display_rect_t){0, 0, 240, 240};
    assert(h2_display_viewport_create(&memory, &backend, &area, &v) == 0);
    b.info_rc = H2_DISPLAY_ERR_IO;
    assert(h2_pal_display_open(h2_display_viewport_api(v)) == H2_DISPLAY_ERR_IO);
    b.info_rc = 0;
    b.open_rc = H2_DISPLAY_ERR_IO;
    assert(h2_pal_display_open(h2_display_viewport_api(v)) == H2_DISPLAY_ERR_IO);
    assert(h2_display_viewport_destroy(v) == 0);
    area.x = INT_MAX;
    assert(h2_display_viewport_create(&memory, &backend, &area, &v) == H2_PAL_ERR_INVALID_ARG && v == NULL);
    area.x = 0;
    const h2_pal_mem_vtable_t fail_ops = {.alloc = no_memory, .free = release};
    const h2_pal_mem_api_t fail_mem = {.vtable = &fail_ops};
    assert(h2_display_viewport_create(&fail_mem, &backend, &area, &v) == H2_PAL_ERR_NO_MEMORY && v == NULL);
    assert(h2_display_viewport_destroy(NULL) == 0 && h2_display_viewport_api(NULL) == NULL);
    return 0;
}
