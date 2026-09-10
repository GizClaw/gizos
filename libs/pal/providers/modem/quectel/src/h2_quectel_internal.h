#ifndef H2_QUECTEL_INTERNAL_H
#define H2_QUECTEL_INTERNAL_H

#include "h2_quectel_modem.h"

typedef struct h2_quectel_response {
    char lines[H2_QUECTEL_RESPONSE_MAX][H2_QUECTEL_LINE_MAX];
    size_t count;
    int connected;
} h2_quectel_response_t;

static inline int h2_quectel_ascii_digit(unsigned char value) {
    return value >= (unsigned char)'0' && value <= (unsigned char)'9';
}

static inline int h2_quectel_ascii_space(unsigned char value) {
    return value == (unsigned char)' ' || value == (unsigned char)'\t' ||
        value == (unsigned char)'\n' || value == (unsigned char)'\r' ||
        value == (unsigned char)'\f' || value == (unsigned char)'\v';
}

h2_quectel_modem_t *h2_quectel_from_platform(h2_pal_modem_t *platform);
h2_pal_result_t h2_quectel_at_exchange(
    h2_quectel_modem_t *modem,
    const char *cmd,
    h2_quectel_response_t *response,
    int allow_connect);
/* Same exchange with a per-command timeout; `timeout_ms == 0` keeps the
 * configured command and I/O timeouts. */
h2_pal_result_t h2_quectel_at_exchange_timeout(
    h2_quectel_modem_t *modem,
    const char *cmd,
    h2_quectel_response_t *response,
    int allow_connect,
    uint32_t timeout_ms);
const char *h2_quectel_response_find(const h2_quectel_response_t *response, const char *prefix);
int h2_quectel_parse_int_after(const char *text, const char *prefix, int *out_value);
void h2_quectel_copy_token(char *dst, size_t dst_len, const char *src);
h2_pal_modem_registration_state_t h2_quectel_parse_registration_stat(int stat);
int h2_quectel_parse_clcc_line(const char *line, h2_pal_modem_call_status_t *out_status);
int32_t h2_quectel_incoming_call_begin(h2_quectel_modem_t *modem);
int32_t h2_quectel_incoming_call_current(const h2_quectel_modem_t *modem);
int32_t h2_quectel_incoming_call_end(h2_quectel_modem_t *modem);
void h2_quectel_handle_urc_line(h2_quectel_modem_t *modem, const char *line);
h2_pal_result_t h2_quectel_modem_prepare(h2_quectel_modem_t *modem);
uint32_t h2_quectel_modem_capabilities(const h2_quectel_modem_t *modem);
int h2_quectel_cell_locate_token_valid(const char *token);
h2_pal_result_t h2_quectel_modem_cell_locate(
    h2_pal_modem_t *platform,
    uint32_t timeout_ms,
    h2_pal_modem_cell_location_t *out_location);
void h2_quectel_post_system_event(
    h2_quectel_modem_t *modem,
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size);

h2_pal_result_t h2_quectel_operation_begin(h2_quectel_modem_t *modem);
h2_pal_result_t h2_quectel_operation_end(h2_quectel_modem_t *modem, h2_pal_result_t result);
h2_pal_result_t h2_quectel_power_wake(h2_quectel_modem_t *modem);
h2_pal_result_t h2_quectel_power_prepare(h2_quectel_modem_t *modem);
h2_pal_result_t h2_quectel_set_power_policy(void *user, h2_pal_modem_power_policy_t policy);
h2_pal_result_t h2_quectel_get_power_status(void *user, h2_pal_modem_power_status_t *out_status);
void h2_quectel_sim_update(h2_quectel_modem_t *modem, h2_pal_modem_sim_state_t state);
void h2_quectel_handle_urc_locked(h2_quectel_modem_t *modem, const char *line);

#endif
