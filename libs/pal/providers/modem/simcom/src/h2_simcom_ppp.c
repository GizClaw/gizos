#include "h2_simcom_internal.h"

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

h2_pal_result_t h2_simcom_modem_set_apn(
    h2_pal_modem_t *platform,
    const h2_pal_modem_apn_config_t *config) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL ||
        config == NULL ||
        config->apn[0] == '\0' ||
        !apn_token_valid(config->apn) ||
        !apn_token_valid(config->username) ||
        !apn_token_valid(config->password)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_simcom_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    char cmd[H2_SIMCOM_LINE_MAX];
    int n = snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", config->apn);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_simcom_at_exchange(modem, cmd, NULL, 0);
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

h2_pal_result_t h2_simcom_modem_data_open(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->data_status.state == H2_PAL_MODEM_DATA_OPEN) {
        return H2_PAL_OK;
    }
    if (modem->config.data_open == NULL) {
        modem->data_status.last_error = H2_PAL_ERR_UNSUPPORTED;
        return H2_PAL_ERR_UNSUPPORTED;
    }
    modem->data_status.state = H2_PAL_MODEM_DATA_OPENING;
    h2_pal_modem_data_status_t status = modem->data_status;
    h2_pal_result_t rc = modem->config.data_open(
        modem->config.transport_user, timeout_ms, &status);
    if (rc == H2_PAL_OK) {
        status.state = H2_PAL_MODEM_DATA_OPEN;
        status.last_error = H2_PAL_OK;
    } else {
        status.state = H2_PAL_MODEM_DATA_CLOSED;
        status.ip4_valid = 0u;
        status.last_error = rc;
    }
    modem->data_status = status;
    if (rc == H2_PAL_OK) {
        h2_simcom_post_system_event(
            modem,
            H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_OPENED,
            &modem->data_status,
            sizeof(modem->data_status));
    } else {
        const h2_pal_modem_event_t event = {
            .result = rc,
        };
        h2_simcom_post_system_event(
            modem,
            H2_PAL_SYSTEM_EVENT_TYPE_MODEM_ERROR,
            &event,
            sizeof(event));
    }
    return rc;
}

h2_pal_result_t h2_simcom_modem_data_close(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->data_status.state == H2_PAL_MODEM_DATA_CLOSED) {
        return H2_PAL_OK;
    }
    h2_pal_result_t result = modem->config.data_close != NULL
        ? modem->config.data_close(modem->config.transport_user, timeout_ms)
        : H2_PAL_ERR_UNSUPPORTED;
    if (result == H2_PAL_OK) {
        h2_simcom_modem_notify_data_closed(modem, H2_PAL_OK);
        return H2_PAL_OK;
    }
    modem->data_status.last_error = result;
    const h2_pal_modem_event_t event = {
        .result = result,
    };
    h2_simcom_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_ERROR,
        &event,
        sizeof(event));
    return result;
}

h2_pal_result_t h2_simcom_modem_get_data_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_data_status_t *out_status) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_status = modem->data_status;
    return H2_PAL_OK;
}

h2_pal_result_t h2_simcom_modem_dial_ppp(h2_simcom_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_simcom_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_simcom_at_exchange(modem, "AT+CGATT=1", NULL, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_simcom_at_exchange(modem, "AT+CGACT=1,1", NULL, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_simcom_at_exchange(modem, "ATD*99***1#", NULL, 1);
    if (rc == H2_PAL_OK) {
        modem->data_status.state = H2_PAL_MODEM_DATA_OPENING;
        modem->data_status.last_error = H2_PAL_OK;
    }
    return rc;
}

h2_pal_result_t h2_simcom_modem_drop_ppp(h2_simcom_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = H2_PAL_OK;
    h2_pal_result_t rc = h2_simcom_at_exchange(modem, "ATH", NULL, 0);
    if (result == H2_PAL_OK && rc != H2_PAL_OK) {
        result = rc;
    }
    rc = h2_simcom_at_exchange(modem, "AT+CGACT=0,1", NULL, 0);
    if (result == H2_PAL_OK && rc != H2_PAL_OK) {
        result = rc;
    }
    modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    modem->data_status.ip4_valid = 0u;
    modem->data_status.last_error = result;
    return result;
}

void h2_simcom_modem_notify_data_closed(
    h2_simcom_modem_t *modem,
    h2_pal_result_t last_error) {
    if (modem == NULL) {
        return;
    }
    const int changed =
        modem->data_status.state != H2_PAL_MODEM_DATA_CLOSED ||
        modem->data_status.ip4_valid != 0u ||
        modem->data_status.last_error != last_error;
    modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    modem->data_status.ip4 = 0u;
    modem->data_status.dns1_ip4 = 0u;
    modem->data_status.dns2_ip4 = 0u;
    modem->data_status.ip4_valid = 0u;
    modem->data_status.last_error = last_error;
    if (!changed) {
        return;
    }
    h2_simcom_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_CLOSED,
        &modem->data_status,
        sizeof(modem->data_status));
}
