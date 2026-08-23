#ifndef H2_SIMCOM_INTERNAL_H
#define H2_SIMCOM_INTERNAL_H

#include "h2_simcom_modem.h"

typedef struct h2_simcom_response {
    char lines[H2_SIMCOM_RESPONSE_MAX][H2_SIMCOM_LINE_MAX];
    size_t count;
    int connected;
} h2_simcom_response_t;

static inline int h2_simcom_ascii_is_digit(unsigned char value) {
    return value >= (unsigned char)'0' && value <= (unsigned char)'9';
}

static inline int h2_simcom_ascii_is_space(unsigned char value) {
    return value == (unsigned char)' ' || value == (unsigned char)'\t' ||
           value == (unsigned char)'\n' || value == (unsigned char)'\r' ||
           value == (unsigned char)'\f' || value == (unsigned char)'\v';
}

h2_simcom_modem_t *h2_simcom_from_platform(h2_pal_modem_t *platform);
h2_pal_result_t h2_simcom_at_exchange(
    h2_simcom_modem_t *modem,
    const char *cmd,
    h2_simcom_response_t *response,
    int allow_connect);
const char *h2_simcom_response_find(const h2_simcom_response_t *response, const char *prefix);
int h2_simcom_parse_int_after(const char *text, const char *prefix, int *out_value);
void h2_simcom_copy_token(char *dst, size_t dst_len, const char *src);
h2_pal_modem_registration_state_t h2_simcom_parse_registration_stat(int stat);
int h2_simcom_parse_clcc_line(const char *line, h2_pal_modem_call_status_t *out_status);
void h2_simcom_handle_urc_line(h2_simcom_modem_t *modem, const char *line);
h2_pal_result_t h2_simcom_modem_prepare(h2_simcom_modem_t *modem);
uint32_t h2_simcom_modem_capabilities(const h2_simcom_modem_t *modem);
void h2_simcom_post_system_event(
    h2_simcom_modem_t *modem,
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size);

#endif
