#include "h2_quectel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_registration_urc(h2_quectel_modem_t *modem, const char *line, h2_pal_modem_status_t *out_status) {
    if (modem == NULL || line == NULL || out_status == NULL) {
        return 0;
    }
    const char *colon = strchr(line, ':');
    if (colon == NULL) {
        return 0;
    }
    char *end = NULL;
    long stat = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || stat < 0 || stat > 5) {
        return 0;
    }
    while (*end == ' ') { end++; }
    /* Notifications are <stat>[,"lac","ci",...]. Two numeric fields
     * are the solicited <n>,<stat> query response, not a notification. */
    if (*end != '\0' && *end != ',') {
        return 0;
    }
    if (*end == ',') {
        end++;
        while (*end == ' ') { end++; }
        if (*end != '"') { return 0; }
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->capabilities = h2_quectel_modem_capabilities(modem);
    out_status->registration = h2_quectel_parse_registration_stat((int)stat);
    out_status->rat = H2_PAL_MODEM_RAT_LTE;
    return 1;
}

static int parse_packet_urc(h2_quectel_modem_t *modem, const char *line, h2_pal_modem_status_t *out_status) {
    int attached = 0;
    if (modem == NULL || out_status == NULL || !h2_quectel_parse_int_after(line, "+CGATT:", &attached)) {
        return 0;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->capabilities = h2_quectel_modem_capabilities(modem);
    out_status->packet = attached != 0
        ? H2_PAL_MODEM_PACKET_ATTACHED
        : H2_PAL_MODEM_PACKET_DETACHED;
    out_status->rat = H2_PAL_MODEM_RAT_LTE;
    return 1;
}

static int parse_signal_urc(const char *line, h2_pal_modem_signal_t *out_signal) {
    if (line == NULL || out_signal == NULL) {
        return 0;
    }
    int csq = 99;
    int ber = 99;
    if (sscanf(line, "+CSQ: %d,%d", &csq, &ber) != 2) {
        return 0;
    }
    memset(out_signal, 0, sizeof(*out_signal));
    out_signal->rssi_dbm = csq == 99 ? 0 : -113 + (2 * csq);
    out_signal->rssi_valid = csq != 99;
    out_signal->ber = ber;
    out_signal->rat = H2_PAL_MODEM_RAT_LTE;
    return 1;
}

void h2_quectel_post_call_status(h2_quectel_modem_t *modem, h2_pal_system_event_type_t type, const h2_pal_modem_call_status_t *status) {
    if (modem == NULL || status == NULL) { return; }
    h2_pal_modem_call_status_t next = *status;
    if (next.direction == H2_PAL_MODEM_CALL_DIRECTION_INCOMING &&
        next.call_id == modem->incoming_call_id) {
        if (next.state == H2_PAL_MODEM_CALL_STATE_INCOMING && modem->incoming_answered) { return; }
        if (next.state == H2_PAL_MODEM_CALL_STATE_ACTIVE ||
            next.state == H2_PAL_MODEM_CALL_STATE_HELD) { modem->incoming_answered = 1u; }
    }
    if (modem->call_status_seen && modem->observed_call.call_id == next.call_id &&
        modem->observed_call.direction == next.direction) {
        if (next.number[0] == '\0') {
            memcpy(next.number, modem->observed_call.number, sizeof(next.number));
        }
        if (modem->dsci_voice_seen && modem->observed_call.state == next.state &&
            strcmp(modem->observed_call.number, next.number) == 0) {
            return;
        }
    }
    /* An unnumbered terminal after a numbered end is the same occurrence. */
    if (next.state == H2_PAL_MODEM_CALL_STATE_ENDED && modem->call_status_seen &&
        modem->observed_call.state == H2_PAL_MODEM_CALL_STATE_ENDED) {
        return;
    }
    modem->call_generation++;
    modem->observed_call = next;
    modem->call_status_seen = 1u;
    if (next.state == H2_PAL_MODEM_CALL_STATE_ENDED) {
        modem->call_hold = 0u;
        (void)h2_quectel_incoming_call_end(modem);
    }
    h2_pal_modem_call_event_t event = {.call = next};
    h2_quectel_post_system_event(modem, type, &event, sizeof(event));
}

static void handle_dsci(h2_quectel_modem_t *modem, const char *line) {
    int id, dir, stat, type, offset = 0;
    if (sscanf(line, "^DSCI: %d , %d , %d , %d , %n", &id, &dir, &stat, &type, &offset) != 4 ||
        offset == 0 || id <= 0 || dir < 0 || dir > 1 || stat < 1 || stat > 7 || type != 0) {
        return;
    }
    const char *number = line + offset;
    const char *comma = strrchr(number, ',');
    int num_type;
    char tail;
    if (comma == NULL || sscanf(comma + 1, " %d %c", &num_type, &tail) != 1) {
        return;
    }
    h2_pal_modem_call_status_t status = {0};
    static const h2_pal_modem_call_state_t states[] = {
        H2_PAL_MODEM_CALL_STATE_IDLE, H2_PAL_MODEM_CALL_STATE_HELD,
        H2_PAL_MODEM_CALL_STATE_DIALING, H2_PAL_MODEM_CALL_STATE_ACTIVE,
        H2_PAL_MODEM_CALL_STATE_INCOMING, H2_PAL_MODEM_CALL_STATE_WAITING,
        H2_PAL_MODEM_CALL_STATE_ENDED, H2_PAL_MODEM_CALL_STATE_ALERTING,
    };
    size_t length = (size_t)(comma - number);
    while (length && number[length - 1u] == ' ') { length--; }
    if (length >= 2u && number[0] == '"' && number[length - 1u] == '"') {
        number++;
        length -= 2u;
    }
    if (length >= sizeof(status.number)) { return; }
    memcpy(status.number, number, length);
    modem->dsci_voice_seen = 1u;
    status.state = states[stat];
    status.direction = dir ? H2_PAL_MODEM_CALL_DIRECTION_INCOMING : H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
    if (stat == 6) {
        /* Ignore duplicate/stale ends, including a previous modem call ID. */
        if ((dir && !modem->incoming_dsci_seen) ||
            (modem->dsci_call_id != 0 && modem->dsci_call_id != id) ||
            (modem->call_status_seen && modem->observed_call.state == H2_PAL_MODEM_CALL_STATE_ENDED)) {
            return;
        }
        status.call_id = dir ? h2_quectel_incoming_call_current(modem) : id;
        if (dir && status.call_id == 0) { return; }
    } else {
        status.call_id = dir ? h2_quectel_incoming_call_begin(modem) : id;
        modem->dsci_call_id = id;
        if (dir) { modem->incoming_dsci_seen = 1u; }
        modem->call_hold = 1u;
        (void)h2_quectel_power_wake(modem);
    }
    h2_quectel_post_call_status(modem,
        stat == 6 ? H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED :
        (dir && stat == 4 ? H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING :
         H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED), &status);
}

void h2_quectel_handle_urc_locked(h2_quectel_modem_t *modem, const char *line) {
    if (modem == NULL || line == NULL || line[0] == '\0') {
        return;
    }

    if (strcmp(line, "+QIND: SMS DONE") == 0 ||
        strcmp(line, "+QIND: PB DONE") == 0 || strcmp(line, "Call Ready") == 0) {
        if (modem->sim_state != H2_PAL_MODEM_SIM_STATE_READY && !modem->sim_hint_seen) {
            modem->sim_hint_seen = 1u;
            modem->sim_query_pending = 1u;
        }
        return;
    }

    if (strncmp(line, "+QSIMSTAT", 9u) == 0) {
        int enabled = -1;
        int inserted = -1;
        char tail = '\0';
        if (sscanf(line + 9, " : %d , %d %c", &enabled, &inserted, &tail) != 2 ||
            enabled < 0 || enabled > 1 || inserted < 0 || inserted > 2) {
            return;
        }
        if (inserted == 0) {
            if (modem->sim_presence != 2u) {
                modem->sim_probe_ticks = 0u;
                modem->sim_hint_seen = 0u;
            }
            modem->sim_presence = 2u;
            modem->sim_poll_remaining = 0u;
            modem->sim_refresh_pending = 0u;
        } else if (inserted == 1 && modem->sim_presence != 1u) {
            modem->sim_presence = 1u;
            modem->sim_poll_remaining = 30u;
        }
        /* Inserted does not mean PIN-ready; repeated insertion indications
         * must not downgrade an already READY/LOCKED card. */
        if (inserted == 1 && modem->sim_seen != 0u &&
            (modem->sim_state == H2_PAL_MODEM_SIM_STATE_READY ||
             modem->sim_state == H2_PAL_MODEM_SIM_STATE_LOCKED)) {
            if (modem->sim_state == H2_PAL_MODEM_SIM_STATE_LOCKED) { modem->sim_poll_remaining = 0u; }
            return;
        }
        h2_quectel_sim_update(modem, inserted == 0 ? H2_PAL_MODEM_SIM_STATE_ABSENT : H2_PAL_MODEM_SIM_STATE_UNKNOWN);
        return;
    }
    if (strncmp(line, "+CPIN:", 6u) == 0) {
        const char *value = line + 6;
        while (*value == ' ') { value++; }
        h2_pal_modem_sim_state_t state = H2_PAL_MODEM_SIM_STATE_UNKNOWN;
        if (strcmp(value, "READY") == 0) {
            state = H2_PAL_MODEM_SIM_STATE_READY;
        } else if (strcmp(value, "SIM PIN") == 0 || strcmp(value, "SIM PUK") == 0) {
            state = H2_PAL_MODEM_SIM_STATE_LOCKED;
        } else if (modem->sim_state == H2_PAL_MODEM_SIM_STATE_ABSENT) {
            state = H2_PAL_MODEM_SIM_STATE_ABSENT;
        }
        /* An unsolicited READY may predate removal. Confirm it with a new
         * probe rather than reviving the removed SIM from an uncorrelated URC. */
        if (modem->sim_presence == 2u) {
            if (state == H2_PAL_MODEM_SIM_STATE_READY) { modem->sim_query_pending = 1u; }
            return;
        }
        if (state == H2_PAL_MODEM_SIM_STATE_READY) {
            modem->sim_presence = 1u;
            modem->sim_probe_ticks = 0u;
            modem->sim_query_pending = 0u;
        }
        h2_quectel_sim_update(modem, state);
        return;
    }
    if (h2_quectel_is_sim_absent_line(line)) {
        h2_quectel_sim_update(modem, H2_PAL_MODEM_SIM_STATE_ABSENT);
        return;
    }

    if (strncmp(line, "^DSCI:", 6u) == 0) {
        handle_dsci(modem, line);
        return;
    }

    if (strcmp(line, "RING") == 0 || strncmp(line, "+CRING:", 7) == 0) {
        modem->call_hold = 1u;
        (void)h2_quectel_power_wake(modem);
        /* RI/transport wakes and retains the host before delivering this URC. */
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        status.call_id = h2_quectel_incoming_call_begin(modem);
        status.direction = H2_PAL_MODEM_CALL_DIRECTION_INCOMING;
        status.state = H2_PAL_MODEM_CALL_STATE_INCOMING;
        h2_quectel_post_call_status(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING, &status);
        return;
    }

    if (strncmp(line, "+CLIP:", 6) == 0) {
        /* A terminal occurrence stays terminal until a new RING/DSCI or a
         * solicited call snapshot establishes the next occurrence. */
        if (!modem->incoming_call_id && modem->call_status_seen &&
            modem->observed_call.state == H2_PAL_MODEM_CALL_STATE_ENDED) { return; }
        const int32_t incoming_call_id = h2_quectel_incoming_call_begin(modem);
        modem->call_hold = 1u;
        (void)h2_quectel_power_wake(modem);
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        status.call_id = incoming_call_id;
        status.direction = H2_PAL_MODEM_CALL_DIRECTION_INCOMING;
        status.state = H2_PAL_MODEM_CALL_STATE_INCOMING;
        const char *quote = strchr(line, '"');
        if (quote != NULL) {
            h2_quectel_copy_token(status.number, sizeof(status.number), quote);
        }
        h2_quectel_post_call_status(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING, &status);
        return;
    }

    if (strncmp(line, "+CLCC:", 6) == 0) {
        h2_pal_modem_call_status_t status;
        if (h2_quectel_parse_clcc_line(line, &status)) {
            if (!modem->incoming_call_id && modem->call_status_seen &&
                modem->observed_call.state == H2_PAL_MODEM_CALL_STATE_ENDED) { return; }
            if (modem->incoming_dsci_seen && modem->call_status_seen &&
                (modem->observed_call.state == H2_PAL_MODEM_CALL_STATE_ENDED ||
                 (modem->dsci_call_id != 0 && modem->dsci_call_id != status.call_id))) {
                return;
            }
            if (status.direction == H2_PAL_MODEM_CALL_DIRECTION_INCOMING) {
                int32_t modem_id = status.call_id;
                status.call_id = h2_quectel_incoming_call_begin(modem);
                modem->incoming_modem_call_id = modem_id;
                modem->call_hold = 1u;
                (void)h2_quectel_power_wake(modem);
            }
            h2_quectel_post_call_status(
                modem,
                status.state == H2_PAL_MODEM_CALL_STATE_INCOMING
                    ? H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING
                    : H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED,
                &status);
        }
        return;
    }

    if (strcmp(line, "NO CARRIER") == 0 ||
        strcmp(line, "BUSY") == 0 ||
        strcmp(line, "NO ANSWER") == 0) {
        /* Once voice DSCI is observed, only its identified end can terminate
         * calls. Unidentified terminal results may belong to an older call. */
        if (modem->dsci_voice_seen) { return; }
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        modem->call_hold = 0u;
        const int32_t incoming_call_id =
            h2_quectel_incoming_call_end(modem);
        status.call_id = incoming_call_id != 0 ? incoming_call_id : -1;
        status.direction = incoming_call_id != 0
            ? H2_PAL_MODEM_CALL_DIRECTION_INCOMING
            : H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
        status.state = H2_PAL_MODEM_CALL_STATE_ENDED;
        h2_quectel_post_call_status(
            modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED, &status);
        return;
    }

    if (strncmp(line, "+CEREG:", 7) == 0 ||
        strncmp(line, "+CREG:", 6) == 0 ||
        strncmp(line, "+CGREG:", 7) == 0) {
        h2_pal_modem_status_t status;
        if (parse_registration_urc(modem, line, &status)) {
            h2_quectel_post_system_event(
                modem,
                H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED,
                &status,
                sizeof(status));
        }
        return;
    }

    if (strncmp(line, "+CGATT:", 7) == 0) {
        h2_pal_modem_status_t status;
        if (parse_packet_urc(modem, line, &status)) {
            h2_quectel_post_system_event(
                modem,
                H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED,
                &status,
                sizeof(status));
        }
        return;
    }

    if (strncmp(line, "+CSQ:", 5) == 0) {
        h2_pal_modem_signal_t signal;
        if (parse_signal_urc(line, &signal)) {
            h2_quectel_post_system_event(
                modem,
                H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED,
                &signal,
                sizeof(signal));
        }
        return;
    }

    if (strcmp(line, "RDY") == 0 || strcmp(line, "APP RDY") == 0) {
        h2_quectel_reset_state(modem);
        h2_quectel_post_system_event(
            modem,
            H2_PAL_SYSTEM_EVENT_TYPE_MODEM_READY,
            NULL,
            0u);
    }
}

/* Only the serialized AT parser calls this after checking the SIM/reset
 * generations captured before sending CPIN. RX notifications use the ordinary
 * URC entry and cannot borrow the active probe's authority. */
void h2_quectel_cpin_response_locked(h2_quectel_modem_t *modem, const char *line,
    uint32_t reset_generation, uint32_t sim_generation) {
    if (reset_generation != modem->reset_generation || sim_generation != modem->sim_generation) { return; }
    if (strncmp(line, "+CPIN:", 6u) == 0) {
        const char *value = line + 6;
        while (*value == ' ') { value++; }
        if (modem->sim_presence == 2u && strcmp(value, "READY") == 0) {
            /* Generations do not identify delayed serial replies. Once a raw
             * CPIN was interrupted, only insertion/reset can restore trust. */
            if (modem->config.command == NULL && modem->raw_cpin_uncertain) { return; }
            modem->sim_presence = 1u;
        }
    }
    h2_quectel_handle_urc_locked(modem, line);
}

void h2_quectel_reset_state(h2_quectel_modem_t *modem) {
    modem->raw_cpin_uncertain = 0u;
    modem->call_generation++;
    modem->dsci_voice_seen = 0u;
    modem->dsci_call_id = 0;
    modem->call_status_seen = 0u;
    (void)h2_quectel_incoming_call_end(modem);
    modem->reset_generation++;
    modem->registration_seen = 0u;
    modem->packet_seen = 0u;
    modem->signal_seen = 0u;
    modem->prepared = 0u;
    modem->power_configured = 0u;
    modem->cell_locate_token_sent = 0u;
    modem->gnss_hold = 0u;
    modem->call_hold = 0u;
    modem->sim_probe_ticks = 0u;
    modem->sim_query_pending = 0u;
    modem->sim_hint_seen = 0u;
    modem->sim_presence = 0u;
    modem->sim_poll_remaining = 0u;
    modem->sim_refresh_pending = 0u;
    modem->sim_seen = 0u;
    h2_quectel_sim_update(modem, H2_PAL_MODEM_SIM_STATE_UNKNOWN);
}

void h2_quectel_sim_update(h2_quectel_modem_t *modem, h2_pal_modem_sim_state_t state) {
    if (modem->sim_seen != 0u && modem->sim_state == state) {
        return;
    }
    if (state == H2_PAL_MODEM_SIM_STATE_READY) {
        modem->sim_refresh_pending = 1u;
    } else {
        modem->sim_refresh_pending = 0u;
    }
    if (state == H2_PAL_MODEM_SIM_STATE_LOCKED) {
        modem->sim_poll_remaining = 0u;
        modem->sim_refresh_pending = 0u;
    }
    modem->sim_seen = 1u;
    modem->sim_state = state;
    h2_pal_modem_status_t status = {0};
    status.capabilities = modem->capabilities;
    status.sim = state;
    if (state != H2_PAL_MODEM_SIM_STATE_READY) {
        modem->sim_generation++;
        memset(&modem->data_status, 0, sizeof(modem->data_status));
        modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
        modem->data_status.last_error = H2_PAL_ERR_UNAVAILABLE;
        modem->data_hold = 0u;
        if (modem->config.invalidate_data != NULL) {
            modem->config.invalidate_data(modem->config.transport_user);
        }
        status.registration = H2_PAL_MODEM_REGISTRATION_OFFLINE;
        status.packet = H2_PAL_MODEM_PACKET_DETACHED;
        h2_quectel_post_system_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED,
            &status, sizeof(status));
        h2_quectel_post_system_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED,
            &status, sizeof(status));
    }
    h2_quectel_post_system_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIM_CHANGED,
        &status, sizeof(status));
}

void h2_quectel_handle_urc_line(h2_quectel_modem_t *modem, const char *line) {
    if (!h2_quectel_is_urc(line, NULL) || h2_quectel_state_lock(modem) != H2_PAL_OK) {
        return;
    }
    if (modem->transport_closed != 0u) {
        h2_quectel_state_unlock(modem);
        return;
    }
    h2_quectel_handle_urc_locked(modem, line);
    if (modem->operation_depth == 0u) {
        (void)h2_quectel_power_reconcile(modem, H2_PAL_OK);
    }
    h2_quectel_state_unlock(modem);
}
