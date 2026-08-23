#ifndef H2_PAL_DISPLAY_H
#define H2_PAL_DISPLAY_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_display_pixel_format {
    H2_DISPLAY_PIXEL_RGB565 = 0,
    H2_DISPLAY_PIXEL_RGB888 = 1,
    H2_DISPLAY_PIXEL_RGB444 = 2,
} h2_display_pixel_format_t;

typedef struct h2_display_rect {
    int x;
    int y;
    int width;
    int height;
} h2_display_rect_t;

typedef struct h2_display_info {
    int width;
    int height;
    h2_display_pixel_format_t native_format;
} h2_display_info_t;

typedef struct h2_pal_display_api h2_pal_display_api_t;
typedef h2_pal_display_api_t h2_pal_display_t;

typedef struct h2_pal_display_vtable {
    int (*open)(void *user);
    int (*get_info)(void *user, h2_display_info_t *info);
    int (*draw_bitmap)(
        void *user,
        const h2_display_rect_t *rect,
        const void *pixels,
        size_t stride_bytes,
        h2_display_pixel_format_t format);
    int (*present)(void *user);
    int (*set_brightness_percent)(void *user, uint32_t percent);
    int (*close)(void *user);
} h2_pal_display_vtable_t;

struct h2_pal_display_api {
    void *user;
    const h2_pal_display_vtable_t *vtable;
};

/* open is idempotent and must succeed before any other display operation. */
static inline int h2_pal_display_open(const h2_pal_display_api_t *display) {
    if (display == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (display->vtable == NULL || display->vtable->open == NULL) {
        return H2_DISPLAY_ERR_UNSUPPORTED;
    }
    return display->vtable->open(display->user);
}

static inline int h2_pal_display_get_info(const h2_pal_display_api_t *display, h2_display_info_t *info) {
    if (display == NULL || display->vtable == NULL || display->vtable->get_info == NULL || info == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    return display->vtable->get_info(display->user, info);
}

static inline int h2_pal_display_draw_bitmap(
    const h2_pal_display_api_t *display,
    const h2_display_rect_t *rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format) {
    if (display == NULL || display->vtable == NULL || display->vtable->draw_bitmap == NULL ||
        rect == NULL || pixels == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (rect->width <= 0 || rect->height <= 0) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    return display->vtable->draw_bitmap(display->user, rect, pixels, stride_bytes, format);
}

static inline int h2_pal_display_present(const h2_pal_display_api_t *display) {
    if (display == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (display->vtable == NULL || display->vtable->present == NULL) {
        return H2_DISPLAY_OK;
    }
    return display->vtable->present(display->user);
}

static inline int h2_pal_display_set_brightness_percent(const h2_pal_display_api_t *display, uint32_t percent) {
    if (display == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (display->vtable == NULL || display->vtable->set_brightness_percent == NULL) {
        return H2_DISPLAY_ERR_UNSUPPORTED;
    }
    return display->vtable->set_brightness_percent(display->user, percent);
}

static inline int h2_pal_display_close(const h2_pal_display_api_t *display) {
    if (display == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (display->vtable == NULL || display->vtable->close == NULL) {
        return H2_DISPLAY_OK;
    }
    return display->vtable->close(display->user);
}

#ifdef __cplusplus
}
#endif

#endif
