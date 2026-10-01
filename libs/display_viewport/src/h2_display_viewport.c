#include "h2_display_viewport.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

struct h2_display_viewport {
    h2_pal_display_api_t api;
    const h2_pal_mem_api_t *mem;
    const h2_pal_display_api_t *backend;
    h2_display_rect_t area;
    h2_display_info_t info;
    bool acquired;
    bool opened;
};

static int viewport_close(void *user) {
    h2_display_viewport_t *v = user;
    if (!v->acquired) return H2_DISPLAY_OK;
    int rc = h2_pal_display_close(v->backend);
    if (rc == H2_DISPLAY_OK) {
        v->acquired = false;
        v->opened = false;
    }
    return rc;
}

static int viewport_open(void *user) {
    h2_display_viewport_t *v = user;
    if (v->opened) return H2_DISPLAY_OK;
    if (v->acquired) return H2_DISPLAY_ERR_INVALID_STATE;
    int rc = h2_pal_display_open(v->backend);
    if (rc != H2_DISPLAY_OK) return rc;
    v->acquired = true;
    h2_display_info_t info = {0};
    rc = h2_pal_display_get_info(v->backend, &info);
    if (rc == H2_DISPLAY_OK &&
        ((int64_t)v->area.x + v->area.width > info.width ||
         (int64_t)v->area.y + v->area.height > info.height)) {
        rc = H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (rc != H2_DISPLAY_OK) {
        int close_rc = viewport_close(v);
        return close_rc == H2_DISPLAY_OK ? rc : close_rc;
    }
    v->info = info;
    v->info.width = v->area.width;
    v->info.height = v->area.height;
    v->opened = true;
    return H2_DISPLAY_OK;
}

static int viewport_info(void *user, h2_display_info_t *info) {
    h2_display_viewport_t *v = user;
    *info = (h2_display_info_t){0};
    if (!v->opened) return H2_DISPLAY_ERR_INVALID_STATE;
    *info = v->info;
    return H2_DISPLAY_OK;
}

static int viewport_draw(void *user, const h2_display_rect_t *rect,
                         const void *pixels, size_t stride,
                         h2_display_pixel_format_t format) {
    h2_display_viewport_t *v = user;
    if (!v->opened) return H2_DISPLAY_ERR_INVALID_STATE;
    if (rect->x < 0 || rect->y < 0 ||
        (int64_t)rect->x + rect->width > v->area.width ||
        (int64_t)rect->y + rect->height > v->area.height) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    size_t pixel_size;
    switch (format) {
    case H2_DISPLAY_PIXEL_RGB565:
    case H2_DISPLAY_PIXEL_RGB444: pixel_size = 2u; break;
    case H2_DISPLAY_PIXEL_RGB888: pixel_size = 3u; break;
    default: return H2_DISPLAY_ERR_UNSUPPORTED;
    }
    if ((size_t)rect->width > SIZE_MAX / pixel_size)
        return H2_DISPLAY_ERR_INVALID_ARG;
    const size_t row = (size_t)rect->width * pixel_size;
    if (stride < row || (size_t)(rect->height - 1) > (SIZE_MAX - row) / stride)
        return H2_DISPLAY_ERR_INVALID_ARG;
    h2_display_rect_t translated = *rect;
    translated.x += v->area.x;
    translated.y += v->area.y;
    return h2_pal_display_draw_bitmap(v->backend, &translated, pixels, stride, format);
}

static int viewport_present(void *user) {
    h2_display_viewport_t *v = user;
    return v->opened ? h2_pal_display_present(v->backend) : H2_DISPLAY_ERR_INVALID_STATE;
}

static int viewport_brightness(void *user, uint32_t percent) {
    h2_display_viewport_t *v = user;
    return v->opened ? h2_pal_display_set_brightness_percent(v->backend, percent)
                     : H2_DISPLAY_ERR_INVALID_STATE;
}

static const h2_pal_display_vtable_t s_vtable = {
    .open = viewport_open, .get_info = viewport_info,
    .draw_bitmap = viewport_draw, .present = viewport_present,
    .set_brightness_percent = viewport_brightness, .close = viewport_close,
};

h2_pal_result_t h2_display_viewport_create(
    const h2_pal_mem_api_t *mem, const h2_pal_display_api_t *backend,
    const h2_display_rect_t *area, h2_display_viewport_t **out_viewport) {
    if (out_viewport == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_viewport = NULL;
    if (mem == NULL || mem->vtable == NULL || mem->vtable->alloc == NULL ||
        mem->vtable->free == NULL || backend == NULL || area == NULL ||
        area->x < 0 || area->y < 0 || area->width <= 0 || area->height <= 0 ||
        (int64_t)area->x + area->width > INT_MAX ||
        (int64_t)area->y + area->height > INT_MAX)
        return H2_PAL_ERR_INVALID_ARG;
    h2_display_viewport_t *v = h2_pal_mem_alloc(mem, sizeof(*v));
    if (v == NULL) return H2_PAL_ERR_NO_MEMORY;
    *v = (h2_display_viewport_t){.api = {.user = v, .vtable = &s_vtable},
                               .mem = mem, .backend = backend, .area = *area};
    *out_viewport = v;
    return H2_PAL_OK;
}

const h2_pal_display_api_t *h2_display_viewport_api(h2_display_viewport_t *v) {
    return v == NULL ? NULL : &v->api;
}

h2_pal_result_t h2_display_viewport_destroy(h2_display_viewport_t *v) {
    if (v == NULL) return H2_PAL_OK;
    int rc = viewport_close(v);
    if (rc != H2_DISPLAY_OK) return (h2_pal_result_t)rc;
    h2_pal_mem_free(v->mem, v);
    return H2_PAL_OK;
}
