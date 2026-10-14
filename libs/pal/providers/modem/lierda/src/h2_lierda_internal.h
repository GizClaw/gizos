#ifndef H2_LIERDA_INTERNAL_H
#define H2_LIERDA_INTERNAL_H

#include "h2_lierda_modem.h"

#include <stdbool.h>

#define H2_LIERDA_RESPONSE_SIZE 512u

struct h2_lierda_modem {
    h2_pal_modem_api_t api;
    h2_lierda_modem_config_t config;
    h2_pal_mutex_t *mutex;
    bool transport_owned;
    bool opened;
    bool initialized;
};

h2_pal_result_t h2_lierda_command(
    h2_lierda_modem_t *modem, const char *command, char *response);
h2_pal_result_t h2_lierda_command_guard(h2_lierda_modem_t *modem);
h2_pal_result_t h2_lierda_read_status(
    h2_lierda_modem_t *modem, h2_pal_modem_status_t *out_status);
h2_pal_result_t h2_lierda_read_identity(
    h2_lierda_modem_t *modem, h2_pal_modem_identity_t *out_identity);
h2_pal_result_t h2_lierda_read_operator(
    h2_lierda_modem_t *modem, h2_pal_modem_operator_t *out_operator);
h2_pal_result_t h2_lierda_read_signal(
    h2_lierda_modem_t *modem, h2_pal_modem_signal_t *out_signal);
bool h2_lierda_apn_valid(const h2_pal_modem_apn_config_t *apn);
h2_pal_result_t h2_lierda_apply_apn(
    h2_lierda_modem_t *modem, const h2_pal_modem_apn_config_t *apn);
h2_pal_result_t h2_lierda_response_line(
    const char *response, const char *prefix, char *out_line, size_t capacity);
bool h2_lierda_decimal(const char **cursor, unsigned *out_value);
bool h2_lierda_comma(const char **cursor);
bool h2_lierda_end(const char *cursor);

#endif
