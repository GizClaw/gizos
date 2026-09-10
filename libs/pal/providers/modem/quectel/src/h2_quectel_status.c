#include "h2_quectel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

h2_pal_modem_registration_state_t h2_quectel_parse_registration_stat(int stat) {
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
    return h2_quectel_parse_registration_stat((int)strtol(comma + 1, NULL, 10));
}

static h2_pal_modem_packet_state_t parse_packet(const char *line) {
    int value = 0;
    if (h2_quectel_parse_int_after(line, "+CGATT:", &value) == 0) {
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

static h2_pal_result_t h2_quectel_modem_prepare_impl(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->prepared != 0u) {
        return H2_PAL_OK;
    }
    const uint32_t reset_generation = modem->reset_generation;
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT", NULL, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    (void)h2_quectel_at_exchange(modem, "ATE0", NULL, 0);
    (void)h2_quectel_at_exchange(modem, "AT+CMEE=2", NULL, 0);
    rc = h2_quectel_at_exchange(modem, "AT+CLIP=1", NULL, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    (void)h2_quectel_at_exchange(modem, "AT+CREG=1", NULL, 0);
    (void)h2_quectel_at_exchange(modem, "AT+CGREG=1", NULL, 0);
    (void)h2_quectel_at_exchange(modem, "AT+CEREG=1", NULL, 0);
    (void)h2_quectel_at_exchange(modem, "AT+QCFG=\"urc/ri/ring\",\"pulse\",2000,1", NULL, 0);
    (void)h2_quectel_at_exchange(modem, "AT+QCFG=\"risignaltype\",\"physical\"", NULL, 0);
    rc = h2_quectel_power_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (reset_generation != modem->reset_generation) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    modem->prepared = 1u;
    h2_quectel_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_READY,
        NULL,
        0u);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_quectel_modem_get_capabilities_impl(
    h2_pal_modem_t *platform,
    uint32_t *out_capabilities) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_capabilities = h2_quectel_modem_capabilities(modem);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_quectel_modem_get_status_impl(
    h2_pal_modem_t *platform,
    h2_pal_modem_status_t *out_status) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_quectel_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->capabilities = h2_quectel_modem_capabilities(modem);
    out_status->rat = H2_PAL_MODEM_RAT_LTE;

    h2_quectel_response_t response;
    rc = h2_quectel_at_exchange(modem, "AT+CPIN?", &response, 0);
    if (rc != H2_PAL_OK && modem->sim_state != H2_PAL_MODEM_SIM_STATE_ABSENT) {
        h2_quectel_sim_update(modem, H2_PAL_MODEM_SIM_STATE_UNKNOWN);
    }
    out_status->sim = modem->sim_state;

    const uint32_t registration_generation = modem->registration_generation;
    rc = h2_quectel_at_exchange(modem, "AT+CEREG?", &response, 0);
    if (rc != H2_PAL_OK) {
        rc = h2_quectel_at_exchange(modem, "AT+CREG?", &response, 0);
    }
    const char *reg_line = NULL;
    if (rc == H2_PAL_OK) {
        reg_line = h2_quectel_response_find(&response, "+CEREG:");
        if (reg_line == NULL) {
            reg_line = h2_quectel_response_find(&response, "+CREG:");
        }
    }
    out_status->registration = reg_line != NULL
        ? parse_registration_line(reg_line)
        : H2_PAL_MODEM_REGISTRATION_UNKNOWN;
    if (registration_generation != modem->registration_generation) {
        out_status->registration = modem->observed_status.registration;
    }

    if (modem->sim_seen != 0u && modem->sim_state != H2_PAL_MODEM_SIM_STATE_READY) {
        out_status->registration = H2_PAL_MODEM_REGISTRATION_OFFLINE;
    }
    h2_quectel_post_system_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED,
        out_status, sizeof(*out_status));

    const uint32_t packet_generation = modem->packet_generation;
    rc = h2_quectel_at_exchange(modem, "AT+CGATT?", &response, 0);
    out_status->packet = rc == H2_PAL_OK
        ? parse_packet(h2_quectel_response_find(&response, "+CGATT:"))
        : H2_PAL_MODEM_PACKET_UNKNOWN;
    if (packet_generation != modem->packet_generation) {
        out_status->packet = modem->observed_status.packet;
    }
    if (modem->data_status.state == H2_PAL_MODEM_DATA_OPEN) {
        out_status->packet = H2_PAL_MODEM_PACKET_CONNECTED;
    }
    out_status->sim = modem->sim_state;
    if (modem->sim_seen != 0u && modem->sim_state != H2_PAL_MODEM_SIM_STATE_READY) {
        out_status->registration = H2_PAL_MODEM_REGISTRATION_OFFLINE;
        out_status->packet = H2_PAL_MODEM_PACKET_DETACHED;
    }
    out_status->registration = modem->observed_status.registration;
    h2_quectel_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED,
        out_status,
        sizeof(*out_status));
    return H2_PAL_OK;
}

static h2_pal_result_t h2_quectel_modem_get_identity_impl(
    h2_pal_modem_t *platform,
    h2_pal_modem_identity_t *out_identity) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_identity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_quectel_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(out_identity, 0, sizeof(*out_identity));
    const uint32_t reset_generation = modem->reset_generation;
    const uint32_t sim_generation = modem->sim_generation;
    h2_quectel_response_t response;
    if (h2_quectel_at_exchange(modem, "AT+CGMI", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_quectel_copy_token(out_identity->manufacturer, sizeof(out_identity->manufacturer), response.lines[0]);
    }
    if (h2_quectel_at_exchange(modem, "AT+CGMM", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_quectel_copy_token(out_identity->model, sizeof(out_identity->model), response.lines[0]);
    }
    if (h2_quectel_at_exchange(modem, "AT+CGMR", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_quectel_copy_token(out_identity->revision, sizeof(out_identity->revision), response.lines[0]);
    }
    if (h2_quectel_at_exchange(modem, "AT+CGSN", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_quectel_copy_token(out_identity->imei, sizeof(out_identity->imei), response.lines[0]);
    }
    if (h2_quectel_at_exchange(modem, "AT+CIMI", &response, 0) == H2_PAL_OK && response.count > 0u) {
        h2_quectel_copy_token(out_identity->imsi, sizeof(out_identity->imsi), response.lines[0]);
    }
    if (reset_generation != modem->reset_generation || sim_generation != modem->sim_generation) {
        memset(out_identity, 0, sizeof(*out_identity));
        return H2_PAL_ERR_INVALID_STATE;
    }
    return out_identity->imei[0] != '\0' || out_identity->model[0] != '\0'
        ? H2_PAL_OK
        : H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t h2_quectel_modem_get_operator_impl(
    h2_pal_modem_t *platform,
    h2_pal_modem_operator_t *out_operator) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_operator == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_operator, 0, sizeof(*out_operator));
    h2_quectel_response_t response;
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+COPS?", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const char *line = h2_quectel_response_find(&response, "+COPS:");
    const char *quote = line != NULL ? strchr(line, '"') : NULL;
    if (quote == NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    h2_quectel_copy_token(out_operator->name, sizeof(out_operator->name), quote);
    out_operator->rat = H2_PAL_MODEM_RAT_LTE;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_quectel_modem_get_signal_impl(
    h2_pal_modem_t *platform,
    h2_pal_modem_signal_t *out_signal) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_signal == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_signal, 0, sizeof(*out_signal));
    h2_quectel_response_t response;
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+CSQ", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const char *line = h2_quectel_response_find(&response, "+CSQ:");
    int csq = 99;
    int ber = 99;
    if (line == NULL || sscanf(line, "+CSQ: %d,%d", &csq, &ber) != 2) {
        return H2_PAL_ERR_FORMAT;
    }
    out_signal->rssi_dbm = csq_to_dbm(csq);
    out_signal->ber = ber;
    out_signal->rat = H2_PAL_MODEM_RAT_LTE;
    h2_quectel_post_system_event(
        modem,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED,
        out_signal,
        sizeof(*out_signal));
    return H2_PAL_OK;
}

h2_pal_result_t h2_quectel_modem_prepare(h2_quectel_modem_t *modem) {
    h2_quectel_modem_t *modem_state = modem;
    h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_quectel_modem_prepare_impl(modem);
    return h2_quectel_operation_end(modem_state, rc);
}

h2_pal_result_t h2_quectel_modem_get_capabilities(
    h2_pal_modem_t *platform,
    uint32_t *out_capabilities) {
    h2_quectel_modem_t *modem_state = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = h2_quectel_state_lock(modem_state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_quectel_modem_get_capabilities_impl(platform, out_capabilities);
    h2_quectel_state_unlock(modem_state);
    return rc;
}

h2_pal_result_t h2_quectel_modem_get_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_status_t *out_status) {
    h2_quectel_modem_t *modem_state = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((modem_state->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
        if (modem_state->opened == 0u) {
            return h2_quectel_operation_end(modem_state, H2_PAL_ERR_CLOSED);
        }
        rc = h2_quectel_modem_prepare(modem_state);
        if (rc != H2_PAL_OK) {
            return h2_quectel_operation_end(modem_state, rc);
        }
    }
    rc = h2_quectel_modem_get_status_impl(platform, out_status);
    return h2_quectel_operation_end(modem_state, rc);
}

h2_pal_result_t h2_quectel_modem_get_identity(
    h2_pal_modem_t *platform,
    h2_pal_modem_identity_t *out_identity) {
    h2_quectel_modem_t *modem_state = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((modem_state->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
        if (modem_state->opened == 0u) {
            return h2_quectel_operation_end(modem_state, H2_PAL_ERR_CLOSED);
        }
        rc = h2_quectel_modem_prepare(modem_state);
        if (rc != H2_PAL_OK) {
            return h2_quectel_operation_end(modem_state, rc);
        }
    }
    rc = h2_quectel_modem_get_identity_impl(platform, out_identity);
    return h2_quectel_operation_end(modem_state, rc);
}

h2_pal_result_t h2_quectel_modem_get_operator(
    h2_pal_modem_t *platform,
    h2_pal_modem_operator_t *out_operator) {
    h2_quectel_modem_t *modem_state = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((modem_state->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
        if (modem_state->opened == 0u) {
            return h2_quectel_operation_end(modem_state, H2_PAL_ERR_CLOSED);
        }
        rc = h2_quectel_modem_prepare(modem_state);
        if (rc != H2_PAL_OK) {
            return h2_quectel_operation_end(modem_state, rc);
        }
    }
    rc = h2_quectel_modem_get_operator_impl(platform, out_operator);
    return h2_quectel_operation_end(modem_state, rc);
}

h2_pal_result_t h2_quectel_modem_get_signal(
    h2_pal_modem_t *platform,
    h2_pal_modem_signal_t *out_signal) {
    h2_quectel_modem_t *modem_state = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((modem_state->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
        if (modem_state->opened == 0u) {
            return h2_quectel_operation_end(modem_state, H2_PAL_ERR_CLOSED);
        }
        rc = h2_quectel_modem_prepare(modem_state);
        if (rc != H2_PAL_OK) {
            return h2_quectel_operation_end(modem_state, rc);
        }
    }
    rc = h2_quectel_modem_get_signal_impl(platform, out_signal);
    return h2_quectel_operation_end(modem_state, rc);
}
