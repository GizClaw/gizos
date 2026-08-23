#include "h2_quectel_internal.h"

#include <stdio.h>
#include <string.h>

static int apn_token_valid(const char *value) {
    if (value == NULL) {
        return 0;
    }
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p == '"' || *p == ',' || *p == '\r' || *p == '\n') {
            return 0;
        }
    }
    return 1;
}

h2_pal_result_t h2_quectel_modem_set_apn(
    h2_pal_modem_t *platform,
    const h2_pal_modem_apn_config_t *config) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL ||
        config == NULL ||
        config->apn[0] == '\0' ||
        !apn_token_valid(config->apn) ||
        !apn_token_valid(config->username) ||
        !apn_token_valid(config->password)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_quectel_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    char cmd[H2_QUECTEL_LINE_MAX];
    int n = snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", config->apn);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_quectel_at_exchange(modem, cmd, NULL, 0);
    if (rc == H2_PAL_OK) {
        n = snprintf(cmd,
            sizeof(cmd),
            "AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\",1",
            config->apn,
            config->username,
            config->password);
        if (n <= 0 || (size_t)n >= sizeof(cmd)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        rc = h2_quectel_at_exchange(modem, cmd, NULL, 0);
    }
    if (rc == H2_PAL_OK) {
        strncpy(modem->last_apn, config->apn, sizeof(modem->last_apn) - 1u);
        modem->last_apn[sizeof(modem->last_apn) - 1u] = '\0';
        strncpy(modem->last_username, config->username, sizeof(modem->last_username) - 1u);
        modem->last_username[sizeof(modem->last_username) - 1u] = '\0';
        strncpy(modem->last_password, config->password, sizeof(modem->last_password) - 1u);
        modem->last_password[sizeof(modem->last_password) - 1u] = '\0';
    }
    return rc;
}

h2_pal_result_t h2_quectel_modem_data_open(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    modem->data_status.last_error = H2_PAL_ERR_UNSUPPORTED;
    return H2_PAL_ERR_UNSUPPORTED;
}

h2_pal_result_t h2_quectel_modem_data_close(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = H2_PAL_OK;
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+QPPPDROP", NULL, 0);
    if (rc != H2_PAL_OK) {
        result = rc;
    }
    rc = h2_quectel_at_exchange(modem, "ATH", NULL, 0);
    if (result == H2_PAL_OK && rc != H2_PAL_OK) {
        result = rc;
    }
    rc = h2_quectel_at_exchange(modem, "AT+CGACT=0,1", NULL, 0);
    if (result == H2_PAL_OK && rc != H2_PAL_OK) {
        result = rc;
    }
    modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    modem->data_status.ip4_valid = 0u;
    modem->data_status.last_error = result;
    return result;
}

h2_pal_result_t h2_quectel_modem_get_data_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_data_status_t *out_status) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_status = modem->data_status;
    return H2_PAL_OK;
}

h2_pal_result_t h2_quectel_modem_dial_ppp(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_quectel_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_quectel_at_exchange(modem, "AT+CGATT=1", NULL, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_quectel_at_exchange(modem, "AT+CGACT=1,1", NULL, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_quectel_at_exchange(modem, "ATD*99***1#", NULL, 1);
    if (rc == H2_PAL_OK) {
        modem->data_status.state = H2_PAL_MODEM_DATA_OPENING;
        modem->data_status.last_error = H2_PAL_OK;
    }
    return rc;
}

h2_pal_result_t h2_quectel_modem_drop_ppp(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = H2_PAL_OK;
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+QPPPDROP", NULL, 0);
    if (rc != H2_PAL_OK) {
        result = rc;
    }
    rc = h2_quectel_at_exchange(modem, "ATH", NULL, 0);
    if (result == H2_PAL_OK && rc != H2_PAL_OK) {
        result = rc;
    }
    rc = h2_quectel_at_exchange(modem, "AT+CGACT=0,1", NULL, 0);
    if (result == H2_PAL_OK && rc != H2_PAL_OK) {
        result = rc;
    }
    modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    modem->data_status.ip4_valid = 0u;
    modem->data_status.last_error = result;
    return result;
}
