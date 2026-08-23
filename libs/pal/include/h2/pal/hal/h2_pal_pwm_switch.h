#ifndef H2_PAL_PWM_SWITCH_H
#define H2_PAL_PWM_SWITCH_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_periph.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_PWM_SWITCH_DUTY_MAX_X100 10000u

typedef struct h2_pal_pwm_switch_vtable {
    h2_pal_result_t (*set_duty)(
        void *user,
        h2_pal_periph_id_t id,
        uint16_t duty_x100);

    h2_pal_result_t (*get_duty)(
        void *user,
        h2_pal_periph_id_t id,
        uint16_t *out_duty_x100);
} h2_pal_pwm_switch_vtable_t;

typedef struct h2_pal_pwm_switch_api {
    void *user;
    const h2_pal_pwm_switch_vtable_t *vtable;
} h2_pal_pwm_switch_api_t;

static inline h2_pal_result_t h2_pal_pwm_switch_set_duty(
    const h2_pal_pwm_switch_api_t *api,
    h2_pal_periph_id_t id,
    uint16_t duty_x100) {
    if (duty_x100 > H2_PAL_PWM_SWITCH_DUTY_MAX_X100) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->set_duty == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_duty(api->user, id, duty_x100);
}

static inline h2_pal_result_t h2_pal_pwm_switch_get_duty(
    const h2_pal_pwm_switch_api_t *api,
    h2_pal_periph_id_t id,
    uint16_t *out_duty_x100) {
    if (out_duty_x100 == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_duty == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_duty(api->user, id, out_duty_x100);
}

#ifdef __cplusplus
}
#endif

#endif
