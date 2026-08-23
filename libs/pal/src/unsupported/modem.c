#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_modem_open(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_close(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_capabilities(void *p0, uint32_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_status(void *p0, h2_pal_modem_status_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_identity(void *p0, h2_pal_modem_identity_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_operator(void *p0, h2_pal_modem_operator_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_set_apn(void *p0, const h2_pal_modem_apn_config_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_data_open(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_data_close(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_data_status(void *p0, h2_pal_modem_data_status_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_signal(void *p0, h2_pal_modem_signal_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_call_dial(void *p0, const h2_pal_modem_call_request_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_call_answer(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_call_hangup(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_call_status(void *p0, h2_pal_modem_call_status_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_gnss_start(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_gnss_stop(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_gnss_state(void *p0, h2_pal_modem_gnss_state_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_modem_get_gnss_fix(void *p0, h2_pal_modem_gnss_fix_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_modem_vtable_t unsupported_modem_vtable = {
    .open = unsupported_modem_open,
    .close = unsupported_modem_close,
    .get_capabilities = unsupported_modem_get_capabilities,
    .get_status = unsupported_modem_get_status,
    .get_identity = unsupported_modem_get_identity,
    .get_operator = unsupported_modem_get_operator,
    .set_apn = unsupported_modem_set_apn,
    .data_open = unsupported_modem_data_open,
    .data_close = unsupported_modem_data_close,
    .get_data_status = unsupported_modem_get_data_status,
    .get_signal = unsupported_modem_get_signal,
    .call_dial = unsupported_modem_call_dial,
    .call_answer = unsupported_modem_call_answer,
    .call_hangup = unsupported_modem_call_hangup,
    .get_call_status = unsupported_modem_get_call_status,
    .gnss_start = unsupported_modem_gnss_start,
    .gnss_stop = unsupported_modem_gnss_stop,
    .get_gnss_state = unsupported_modem_get_gnss_state,
    .get_gnss_fix = unsupported_modem_get_gnss_fix,
};
static const h2_pal_modem_api_t unsupported_modem_api = { .user = NULL, .vtable = &unsupported_modem_vtable };
const h2_pal_modem_api_t *h2_pal_unsupported_modem_api(void) { return &unsupported_modem_api; }
