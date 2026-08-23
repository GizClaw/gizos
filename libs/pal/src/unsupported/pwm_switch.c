#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_pwm_switch_set_duty(void *p0, h2_pal_periph_id_t p1, uint16_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_pwm_switch_get_duty(void *p0, h2_pal_periph_id_t p1, uint16_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_pwm_switch_vtable_t unsupported_pwm_switch_vtable = {
    .set_duty = unsupported_pwm_switch_set_duty,
    .get_duty = unsupported_pwm_switch_get_duty,
};
static const h2_pal_pwm_switch_api_t unsupported_pwm_switch_api = { .user = NULL, .vtable = &unsupported_pwm_switch_vtable };
const h2_pal_pwm_switch_api_t *h2_pal_unsupported_pwm_switch_api(void) { return &unsupported_pwm_switch_api; }
