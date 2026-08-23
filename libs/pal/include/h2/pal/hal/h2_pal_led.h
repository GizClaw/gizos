#ifndef H2_PAL_LED_H
#define H2_PAL_LED_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_periph.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef h2_pal_periph_id_t h2_pal_led_id_t;

typedef struct h2_pal_led_color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t w;
} h2_pal_led_color_t;

typedef struct h2_pal_led_info {
    h2_pal_led_id_t id;
    uint16_t pixel_count;
    uint8_t channels_per_pixel;
    uint8_t supports_brightness;
    uint8_t frame_commit_is_atomic;
    uint8_t reserved[3];
} h2_pal_led_info_t;

typedef struct h2_pal_led_vtable {
    h2_pal_result_t (*get_info)(
        void *user,
        h2_pal_led_id_t id,
        h2_pal_led_info_t *out_info);

    h2_pal_result_t (*set_frame)(
        void *user,
        h2_pal_led_id_t id,
        const h2_pal_led_color_t *pixels,
        size_t pixel_count);

    h2_pal_result_t (*set_solid)(
        void *user,
        h2_pal_led_id_t id,
        h2_pal_led_color_t color);

    h2_pal_result_t (*clear)(
        void *user,
        h2_pal_led_id_t id);

    h2_pal_result_t (*set_brightness_percent)(
        void *user,
        h2_pal_led_id_t id,
        uint8_t percent);
} h2_pal_led_vtable_t;

typedef struct h2_pal_led_api {
    void *user;
    const h2_pal_led_vtable_t *vtable;
} h2_pal_led_api_t;

static inline uint32_t h2_pal_led_source_id(h2_pal_led_id_t id) {
    return h2_pal_periph_source_id(id);
}

static inline h2_pal_result_t h2_pal_led_get_info(
    const h2_pal_led_api_t *api,
    h2_pal_led_id_t id,
    h2_pal_led_info_t *out_info) {
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_info == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_info(api->user, id, out_info);
}

static inline h2_pal_result_t h2_pal_led_set_frame(
    const h2_pal_led_api_t *api,
    h2_pal_led_id_t id,
    const h2_pal_led_color_t *pixels,
    size_t pixel_count) {
    if (pixels == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->set_frame == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_frame(api->user, id, pixels, pixel_count);
}

static inline h2_pal_result_t h2_pal_led_set_solid(
    const h2_pal_led_api_t *api,
    h2_pal_led_id_t id,
    h2_pal_led_color_t color) {
    if (api == NULL || api->vtable == NULL || api->vtable->set_solid == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_solid(api->user, id, color);
}

static inline h2_pal_result_t h2_pal_led_clear(
    const h2_pal_led_api_t *api,
    h2_pal_led_id_t id) {
    if (api == NULL || api->vtable == NULL || api->vtable->clear == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->clear(api->user, id);
}

static inline h2_pal_result_t h2_pal_led_set_brightness_percent(
    const h2_pal_led_api_t *api,
    h2_pal_led_id_t id,
    uint8_t percent) {
    if (percent > 100u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->set_brightness_percent == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_brightness_percent(api->user, id, percent);
}

#ifdef __cplusplus
}
#endif

#endif
