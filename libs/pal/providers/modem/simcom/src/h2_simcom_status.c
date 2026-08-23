#include "h2_simcom_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_pal_modem_sim_state_t parse_sim(const char *line) {
    if (line == NULL) {
        return H2_PAL_MODEM_SIM_STATE_UNKNOWN;
    }
    const char *state = strchr(line, ':');
    state = state != NULL ? state + 1 : line;
    while (*state == ' ') {
        ++state;
    }
    if (strcmp(state, "READY") == 0) {
        return H2_PAL_MODEM_SIM_STATE_READY;
    }
    if (strstr(state, "PIN") != NULL || strstr(state, "PUK") != NULL) {
        return H2_PAL_MODEM_SIM_STATE_LOCKED;
    }
    return H2_PAL_MODEM_SIM_STATE_UNKNOWN;
}

static int decimal_id_valid(const char *value, size_t expected_len) {
    if (value == NULL || strlen(value) != expected_len) {
        return 0;
    }
    for (size_t i = 0u; i < expected_len; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return 0;
        }
    }
    return 1;
}

h2_pal_modem_registration_state_t h2_simcom_parse_registration_stat(int stat) {
    switch (stat) {
        case 1:
            return H2_PAL_MODEM_REGISTRATION_HOME;
        case 2:
            return H2_PAL_MODEM_REGISTRATION_SEARCHING;
        case 3:
            return H2_PAL_MODEM_REGISTRATION_DENIED;
        case 5:
            return H2_PAL_MODEM_REGISTRATION_ROAMING;
        case 0:
            return H2_PAL_MODEM_REGISTRATION_OFFLINE;
        default:
            return H2_PAL_MODEM_REGISTRATION_UNKNOWN;
    }
}

static h2_pal_modem_registration_state_t parse_registration_line(const char *line) {
    if (line == NULL) {
        return H2_PAL_MODEM_REGISTRATION_UNKNOWN;
    }
    const char *comma = strchr(line, ',');
    if (comma == NULL) {
        return H2_PAL_MODEM_REGISTRATION_UNKNOWN;
    }
    return h2_simcom_parse_registration_stat((int)strtol(comma + 1, NULL, 10));
}

static h2_pal_modem_packet_state_t parse_packet(const char *line) {
    int value = 0;
    if (h2_simcom_parse_int_after(line, "+CGATT:", &value) == 0) {
        return H2_PAL_MODEM_PACKET_UNKNOWN;
    }
    return value != 0 ? H2_PAL_MODEM_PACKET_ATTACHED : H2_PAL_MODEM_PACKET_DETACHED;
}

static int csq_to_dbm(int csq) {
    if (csq == 99) {
        return 0;
    }
    return -113 + (2 * csq);
}

h2_pal_result_t h2_simcom_modem_prepare(h2_simcom_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->prepared != 0u) {
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_simcom_at_exchange(modem, "AT", NULL, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    (void)h2_simcom_at_exchange(modem, "ATE0", NULL, 0);
    (void)h2_simcom_at_exchange(modem, "AT+CMEE=2", NULL, 0);
    (void)h2_simcom_at_exchange(modem, "AT+CLIP=1", NULL, 0);
    (void)h2_simcom_at_exchange(modem, "AT+CREG=1", NULL, 0);
    (void)h2_simcom_at_exchange(modem, "AT+CGREG=1", NULL, 0);
    (void)h2_simcom_at_exchange(modem, "AT+CEREG=1", NULL, 0);
    modem->prepared = 1u;
    h2_simcom_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_READY,
        NULL,
        0u);
    return H2_PAL_OK;
}

h2_pal_result_t h2_simcom_modem_get_capabilities(
    h2_pal_modem_t *platform,
    uint32_t *out_capabilities) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL || out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_capabilities = h2_simcom_modem_capabilities(modem);
    return H2_PAL_OK;
}

h2_pal_result_t h2_simcom_modem_get_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_status_t *out_status) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_simcom_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->capabilities = h2_simcom_modem_capabilities(modem);
    out_status->rat = H2_PAL_MODEM_RAT_LTE;

    h2_simcom_response_t response;
    rc = h2_simcom_at_exchange(modem, "AT+CPIN?", &response, 0);
    if (rc == H2_PAL_OK) {
        out_status->sim = parse_sim(h2_simcom_response_find(&response, "+CPIN:"));
    } else if (h2_simcom_response_find(&response, "+CME ERROR: 10") != NULL ||
               h2_simcom_response_find(&response, "+CME ERROR: SIM not inserted") != NULL) {
        out_status->sim = H2_PAL_MODEM_SIM_STATE_ABSENT;
    } else {
        out_status->sim = H2_PAL_MODEM_SIM_STATE_UNKNOWN;
    }

    h2_simcom_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIM_CHANGED,
        out_status,
        sizeof(*out_status));
    if (out_status->sim != H2_PAL_MODEM_SIM_STATE_READY) {
        return H2_PAL_OK;
    }

    rc = h2_simcom_at_exchange(modem, "AT+CEREG?", &response, 0);
    if (rc != H2_PAL_OK) {
        rc = h2_simcom_at_exchange(modem, "AT+CREG?", &response, 0);
    }
    const char *reg_line = NULL;
    if (rc == H2_PAL_OK) {
        reg_line = h2_simcom_response_find(&response, "+CEREG:");
        if (reg_line == NULL) {
            reg_line = h2_simcom_response_find(&response, "+CREG:");
        }
    }
    out_status->registration = reg_line != NULL
        ? parse_registration_line(reg_line)
        : H2_PAL_MODEM_REGISTRATION_UNKNOWN;

    rc = h2_simcom_at_exchange(modem, "AT+CGATT?", &response, 0);
    out_status->packet = rc == H2_PAL_OK
        ? parse_packet(h2_simcom_response_find(&response, "+CGATT:"))
        : H2_PAL_MODEM_PACKET_UNKNOWN;
    if (modem->data_status.state == H2_PAL_MODEM_DATA_OPEN) {
        out_status->packet = H2_PAL_MODEM_PACKET_CONNECTED;
    }
    h2_simcom_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED,
        out_status,
        sizeof(*out_status));
    h2_simcom_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED,
        out_status,
        sizeof(*out_status));
    return H2_PAL_OK;
}

h2_pal_result_t h2_simcom_modem_get_identity(
    h2_pal_modem_t *platform,
    h2_pal_modem_identity_t *out_identity) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL || out_identity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_simcom_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(out_identity, 0, sizeof(*out_identity));
    h2_simcom_response_t response;
    if (h2_simcom_at_exchange(modem, "AT+CGMI", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_simcom_copy_token(out_identity->manufacturer, sizeof(out_identity->manufacturer), response.lines[0]);
    }
    if (h2_simcom_at_exchange(modem, "AT+CGMM", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_simcom_copy_token(out_identity->model, sizeof(out_identity->model), response.lines[0]);
    }
    if (h2_simcom_at_exchange(modem, "AT+CGMR", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_simcom_copy_token(out_identity->revision, sizeof(out_identity->revision), response.lines[0]);
    }
    if (h2_simcom_at_exchange(modem, "AT+CGSN", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_simcom_copy_token(out_identity->imei, sizeof(out_identity->imei), response.lines[0]);
    }
    if (h2_simcom_at_exchange(modem, "AT+CIMI", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_simcom_copy_token(out_identity->imsi, sizeof(out_identity->imsi), response.lines[0]);
    }
    if (out_identity->manufacturer[0] == '\0' ||
        out_identity->model[0] == '\0' ||
        out_identity->revision[0] == '\0' ||
        out_identity->imei[0] == '\0') {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    return decimal_id_valid(out_identity->imei, 15u)
        ? H2_PAL_OK
        : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_simcom_modem_get_operator(
    h2_pal_modem_t *platform,
    h2_pal_modem_operator_t *out_operator) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL || out_operator == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_operator, 0, sizeof(*out_operator));
    h2_simcom_response_t response;
    h2_pal_result_t rc = h2_simcom_at_exchange(modem, "AT+COPS?", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const char *line = h2_simcom_response_find(&response, "+COPS:");
    const char *quote = line != NULL ? strchr(line, '"') : NULL;
    if (quote == NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    h2_simcom_copy_token(out_operator->name, sizeof(out_operator->name), quote);
    out_operator->rat = H2_PAL_MODEM_RAT_LTE;
    return H2_PAL_OK;
}

h2_pal_result_t h2_simcom_modem_get_signal(
    h2_pal_modem_t *platform,
    h2_pal_modem_signal_t *out_signal) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL || out_signal == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_signal, 0, sizeof(*out_signal));
    h2_simcom_response_t response;
    h2_pal_result_t rc = h2_simcom_at_exchange(modem, "AT+CSQ", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const char *line = h2_simcom_response_find(&response, "+CSQ:");
    int csq = 99;
    int ber = 99;
    if (line == NULL || sscanf(line, "+CSQ: %d,%d", &csq, &ber) != 2) {
        return H2_PAL_ERR_FORMAT;
    }
    out_signal->rssi_dbm = csq_to_dbm(csq);
    out_signal->ber = ber;
    out_signal->rat = H2_PAL_MODEM_RAT_LTE;
    h2_simcom_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED,
        out_signal,
        sizeof(*out_signal));
    return H2_PAL_OK;
}
