#ifndef H2_PAL_SWITCH_H
#define H2_PAL_SWITCH_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_periph.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_switch_state {
    H2_PAL_SWITCH_STATE_OFF = 0,
    H2_PAL_SWITCH_STATE_ON = 1,
} h2_pal_switch_state_t;

typedef struct h2_pal_switch_vtable {
    h2_pal_result_t (*set)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_switch_state_t state);

    h2_pal_result_t (*get)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_switch_state_t *out_state);
} h2_pal_switch_vtable_t;

typedef struct h2_pal_switch_api {
    void *user;
    const h2_pal_switch_vtable_t *vtable;
} h2_pal_switch_api_t;

static inline uint32_t h2_pal_switch_source_id(h2_pal_periph_id_t id) {
    return h2_pal_periph_source_id(id);
}

static inline int h2_pal_switch_state_is_valid(h2_pal_switch_state_t state) {
    return state == H2_PAL_SWITCH_STATE_OFF ||
           state == H2_PAL_SWITCH_STATE_ON;
}

static inline h2_pal_result_t h2_pal_switch_set(
    const h2_pal_switch_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_switch_state_t state) {
    if (!h2_pal_switch_state_is_valid(state)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->set == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set(api->user, id, state);
}

static inline h2_pal_result_t h2_pal_switch_get(
    const h2_pal_switch_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_switch_state_t *out_state) {
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get(api->user, id, out_state);
}

#ifdef __cplusplus
}
#endif

#endif
