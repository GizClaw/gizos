#include "h2_lierda_internal.h"

#include <string.h>

static h2_pal_modem_registration_state_t registration(unsigned value) {
    switch (value) {
        case 0u: return H2_PAL_MODEM_REGISTRATION_OFFLINE;
        case 1u: return H2_PAL_MODEM_REGISTRATION_HOME;
        case 2u: return H2_PAL_MODEM_REGISTRATION_SEARCHING;
        case 3u: return H2_PAL_MODEM_REGISTRATION_DENIED;
        case 5u: return H2_PAL_MODEM_REGISTRATION_ROAMING;
        default: return H2_PAL_MODEM_REGISTRATION_UNKNOWN;
    }
}

h2_pal_result_t h2_lierda_read_status(
    h2_lierda_modem_t *modem, h2_pal_modem_status_t *out_status) {
    char response[H2_LIERDA_RESPONSE_SIZE];
    char line[H2_LIERDA_RESPONSE_SIZE];
    h2_pal_modem_status_t status = {.capabilities = H2_PAL_MODEM_CAPABILITY_DATA};
    h2_pal_result_t rc = h2_lierda_command(modem, "AT+CPIN?", response);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_lierda_response_line(response, "+CPIN:", line, sizeof(line));
    if (rc != H2_PAL_OK) return rc;
    const char *sim = line;
    while (*sim == ' ' || *sim == '\t') ++sim;
    if (strcmp(sim, "READY") == 0) status.sim = H2_PAL_MODEM_SIM_STATE_READY;
    else if (strstr(sim, "PIN") != NULL || strstr(sim, "PUK") != NULL) {
        status.sim = H2_PAL_MODEM_SIM_STATE_LOCKED;
    } else status.sim = H2_PAL_MODEM_SIM_STATE_UNKNOWN;
    /* No vendor-specific absent-SIM error/URC guess. */
    if (status.sim != H2_PAL_MODEM_SIM_STATE_READY) {
        *out_status = status;
        return H2_PAL_OK;
    }
    rc = h2_lierda_command(modem, "AT+CEREG?", response);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_lierda_response_line(response, "+CEREG:", line, sizeof(line));
    if (rc != H2_PAL_OK) return rc;
    const char *cursor = line;
    unsigned mode, state;
    if (!h2_lierda_decimal(&cursor, &mode) || mode > 5u ||
        !h2_lierda_comma(&cursor) || !h2_lierda_decimal(&cursor, &state) ||
        state > 10u || (!h2_lierda_end(cursor) && *cursor != ',')) {
        return H2_PAL_ERR_FORMAT;
    }
    status.registration = registration(state);
    /* Optional location/AcT fields are not required for a data-only profile. */
    status.rat = H2_PAL_MODEM_RAT_UNKNOWN;
    rc = h2_lierda_command(modem, "AT+CGATT?", response);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_lierda_response_line(response, "+CGATT:", line, sizeof(line));
    if (rc != H2_PAL_OK) return rc;
    cursor = line;
    if (!h2_lierda_decimal(&cursor, &state) || state > 1u ||
        !h2_lierda_end(cursor)) return H2_PAL_ERR_FORMAT;
    status.packet = state == 1u ? H2_PAL_MODEM_PACKET_ATTACHED
                              : H2_PAL_MODEM_PACKET_DETACHED;
    *out_status = status;
    return H2_PAL_OK;
}

static h2_pal_result_t identity_field(
    h2_lierda_modem_t *modem, const char *command, char *out_value) {
    char response[H2_LIERDA_RESPONSE_SIZE];
    h2_pal_result_t rc = h2_lierda_command(modem, command, response);
    return rc == H2_PAL_OK ? h2_lierda_response_line(
        response, NULL, out_value, H2_PAL_MODEM_IDENTITY_MAX) : rc;
}

static bool decimal_identity(const char *value, size_t minimum, size_t maximum) {
    const size_t len = strlen(value);
    if (len < minimum || len > maximum) return false;
    for (size_t i = 0u; i < len; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

h2_pal_result_t h2_lierda_read_identity(
    h2_lierda_modem_t *modem, h2_pal_modem_identity_t *out_identity) {
    h2_pal_modem_identity_t identity = {0};
    h2_pal_result_t rc = identity_field(modem, "AT+CGMI", identity.manufacturer);
    if (rc == H2_PAL_OK) rc = identity_field(modem, "AT+CGMM", identity.model);
    if (rc == H2_PAL_OK) rc = identity_field(modem, "AT+CGMR", identity.revision);
    if (rc == H2_PAL_OK) rc = identity_field(modem, "AT+CGSN", identity.imei);
    if (rc != H2_PAL_OK) return rc;
    if (!decimal_identity(identity.imei, 15u, 15u)) return H2_PAL_ERR_FORMAT;
    rc = identity_field(modem, "AT+CIMI", identity.imsi);
    if (rc != H2_PAL_OK) return rc;
    if (!decimal_identity(identity.imsi, 6u, 15u)) return H2_PAL_ERR_FORMAT;
    *out_identity = identity;
    return H2_PAL_OK;
}

h2_pal_result_t h2_lierda_read_signal(
    h2_lierda_modem_t *modem, h2_pal_modem_signal_t *out_signal) {
    char response[H2_LIERDA_RESPONSE_SIZE];
    char line[H2_LIERDA_RESPONSE_SIZE];
    h2_pal_result_t rc = h2_lierda_command(modem, "AT+CSQ", response);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_lierda_response_line(response, "+CSQ:", line, sizeof(line));
    if (rc != H2_PAL_OK) return rc;
    const char *cursor = line;
    unsigned rssi, ber;
    if (!h2_lierda_decimal(&cursor, &rssi) || !h2_lierda_comma(&cursor) ||
        !h2_lierda_decimal(&cursor, &ber) || !h2_lierda_end(cursor) ||
        (ber > 7u && ber != 99u)) return H2_PAL_ERR_FORMAT;
    out_signal->ber = (int32_t)ber;
    if (rssi <= 31u) {
        out_signal->rssi_valid = 1u;
        out_signal->rssi_dbm = -113 + 2 * (int32_t)rssi;
    }
    /* No unqualified vendor RSRP command or inferred RAT. */
    return H2_PAL_OK;
}

h2_pal_result_t h2_lierda_read_operator(
    h2_lierda_modem_t *modem, h2_pal_modem_operator_t *out_operator) {
    char response[H2_LIERDA_RESPONSE_SIZE];
    char line[H2_LIERDA_RESPONSE_SIZE];
    h2_pal_result_t rc = h2_lierda_command(modem, "AT+COPS?", response);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_lierda_response_line(response, "+COPS:", line, sizeof(line));
    if (rc != H2_PAL_OK) return rc;
    const char *cursor = line;
    unsigned mode, format;
    if (!h2_lierda_decimal(&cursor, &mode) || mode > 4u) return H2_PAL_ERR_FORMAT;
    if (h2_lierda_end(cursor)) return H2_PAL_OK;
    if (!h2_lierda_comma(&cursor) || !h2_lierda_decimal(&cursor, &format) ||
        format > 2u || !h2_lierda_comma(&cursor) || *cursor++ != '"') {
        return H2_PAL_ERR_FORMAT;
    }
    const char *end = strchr(cursor, '"');
    if (end == NULL) return H2_PAL_ERR_FORMAT;
    const size_t len = (size_t)(end - cursor);
    if (len >= sizeof(out_operator->name)) return H2_PAL_ERR_TRUNCATED;
    cursor = end + 1;
    if (!h2_lierda_end(cursor)) {
        unsigned act;
        if (!h2_lierda_comma(&cursor) || !h2_lierda_decimal(&cursor, &act) ||
            !h2_lierda_end(cursor)) return H2_PAL_ERR_FORMAT;
        if (act == 7u) out_operator->rat = H2_PAL_MODEM_RAT_LTE;
    }
    memcpy(out_operator->name, end - len, len);
    out_operator->name[len] = '\0';
    return H2_PAL_OK;
}
