#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_buzzer_get_info(
    void *p0,
    h2_pal_buzzer_id_t p1,
    h2_pal_buzzer_info_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_buzzer_start(
    void *p0,
    h2_pal_buzzer_id_t p1,
    uint32_t p2,
    uint8_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_buzzer_stop(
    void *p0,
    h2_pal_buzzer_id_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_buzzer_vtable_t unsupported_buzzer_vtable = {
    .get_info = unsupported_buzzer_get_info,
    .start = unsupported_buzzer_start,
    .stop = unsupported_buzzer_stop,
};
static const h2_pal_buzzer_api_t unsupported_buzzer_api = {
    .user = NULL,
    .vtable = &unsupported_buzzer_vtable,
};
const h2_pal_buzzer_api_t *h2_pal_unsupported_buzzer_api(void) {
    return &unsupported_buzzer_api;
}
