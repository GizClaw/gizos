#ifndef H2_PAL_BUTTON_H
#define H2_PAL_BUTTON_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_periph.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_button_state {
    H2_PAL_BUTTON_STATE_RELEASED = 0,
    H2_PAL_BUTTON_STATE_PRESSED = 1,
} h2_pal_button_state_t;

typedef struct h2_pal_single_button_reading {
    h2_pal_periph_id_t id;
    h2_pal_button_state_t state;
} h2_pal_single_button_reading_t;

typedef struct h2_pal_radio_button_group_reading {
    h2_pal_periph_id_t id;
    h2_pal_periph_id_t pressed_button_id;
} h2_pal_radio_button_group_reading_t;

typedef struct h2_pal_button_vtable {
    h2_pal_result_t (*read_single_button)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_single_button_reading_t *out_reading);

    h2_pal_result_t (*read_radio_button_group)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_radio_button_group_reading_t *out_reading);
} h2_pal_button_vtable_t;

typedef struct h2_pal_button_api {
    void *user;
    const h2_pal_button_vtable_t *vtable;
} h2_pal_button_api_t;

static inline int h2_pal_button_state_is_valid(h2_pal_button_state_t state) {
    return state == H2_PAL_BUTTON_STATE_RELEASED ||
           state == H2_PAL_BUTTON_STATE_PRESSED;
}

static inline h2_pal_result_t h2_pal_button_read_single_button(
    const h2_pal_button_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t *out_reading) {
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->read_single_button == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read_single_button(api->user, id, out_reading);
}

static inline h2_pal_result_t h2_pal_button_read_radio_button_group(
    const h2_pal_button_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t *out_reading) {
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->read_radio_button_group == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read_radio_button_group(api->user, id, out_reading);
}

#ifdef __cplusplus
}
#endif

#endif
