#include "h2_quectel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_registration_urc(h2_quectel_modem_t *modem, const char *line, h2_pal_modem_status_t *out_status) {
    if (modem == NULL || line == NULL || out_status == NULL) {
        return 0;
    }
    const char *comma = strchr(line, ',');
    int stat = 0;
    if (comma != NULL) {
        stat = (int)strtol(comma + 1, NULL, 10);
    } else {
        const char *colon = strchr(line, ':');
        if (colon == NULL) {
            return 0;
        }
        stat = (int)strtol(colon + 1, NULL, 10);
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->capabilities = h2_quectel_modem_capabilities(modem);
    out_status->registration = h2_quectel_parse_registration_stat(stat);
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
    out_signal->ber = ber;
    out_signal->rat = H2_PAL_MODEM_RAT_LTE;
    return 1;
}

static void post_call_status(h2_quectel_modem_t *modem, h2_pal_system_event_type_t type, const h2_pal_modem_call_status_t *status) {
    if (modem == NULL || status == NULL) {
        return;
    }
    h2_pal_modem_call_event_t event;
    memset(&event, 0, sizeof(event));
    event.call = *status;
    h2_quectel_post_system_event(modem, type, &event, sizeof(event));
}

void h2_quectel_handle_urc_line(h2_quectel_modem_t *modem, const char *line) {
    if (modem == NULL || line == NULL || line[0] == '\0') {
        return;
    }

    if (strcmp(line, "RING") == 0 || strncmp(line, "+CRING:", 7) == 0) {
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        status.call_id = h2_quectel_incoming_call_begin(modem);
        status.direction = H2_PAL_MODEM_CALL_DIRECTION_INCOMING;
        status.state = H2_PAL_MODEM_CALL_STATE_INCOMING;
        post_call_status(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING, &status);
        return;
    }

    if (strncmp(line, "+CLIP:", 6) == 0) {
        const int32_t incoming_call_id =
            h2_quectel_incoming_call_current(modem);
        if (incoming_call_id == 0) {
            return;
        }
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        status.call_id = incoming_call_id;
        status.direction = H2_PAL_MODEM_CALL_DIRECTION_INCOMING;
        status.state = H2_PAL_MODEM_CALL_STATE_INCOMING;
        const char *quote = strchr(line, '"');
        if (quote != NULL) {
            h2_quectel_copy_token(status.number, sizeof(status.number), quote);
        }
        post_call_status(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING, &status);
        return;
    }

    if (strncmp(line, "+CLCC:", 6) == 0) {
        h2_pal_modem_call_status_t status;
        if (h2_quectel_parse_clcc_line(line, &status)) {
            if (status.direction == H2_PAL_MODEM_CALL_DIRECTION_INCOMING) {
                status.call_id = h2_quectel_incoming_call_current(modem);
                if (status.call_id == 0) {
                    return;
                }
            }
            post_call_status(
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
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        const int32_t incoming_call_id =
            h2_quectel_incoming_call_end(modem);
        status.call_id = incoming_call_id != 0 ? incoming_call_id : -1;
        status.direction = incoming_call_id != 0
            ? H2_PAL_MODEM_CALL_DIRECTION_INCOMING
            : H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
        status.state = H2_PAL_MODEM_CALL_STATE_ENDED;
        post_call_status(
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

    if (strncmp(line, "RDY", 3) == 0 || strncmp(line, "APP RDY", 7) == 0) {
        h2_quectel_post_system_event(
            modem,
            H2_PAL_SYSTEM_EVENT_TYPE_MODEM_READY,
            NULL,
            0u);
    }
}
