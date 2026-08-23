#include "h2_quectel_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_pal_modem_call_state_t parse_clcc_state(int state) {
    switch (state) {
        case 0:
            return H2_PAL_MODEM_CALL_STATE_ACTIVE;
        case 1:
            return H2_PAL_MODEM_CALL_STATE_HELD;
        case 2:
            return H2_PAL_MODEM_CALL_STATE_DIALING;
        case 3:
            return H2_PAL_MODEM_CALL_STATE_ALERTING;
        case 4:
            return H2_PAL_MODEM_CALL_STATE_INCOMING;
        case 5:
            return H2_PAL_MODEM_CALL_STATE_WAITING;
        default:
            return H2_PAL_MODEM_CALL_STATE_IDLE;
    }
}

int32_t h2_quectel_incoming_call_begin(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return 0;
    }
    if (modem->incoming_call_id != 0) {
        return modem->incoming_call_id;
    }
    if (modem->next_incoming_call_id <= 0) {
        modem->next_incoming_call_id = 1;
    }
    modem->incoming_call_id = modem->next_incoming_call_id;
    modem->next_incoming_call_id = modem->next_incoming_call_id == INT32_MAX
        ? 1
        : modem->next_incoming_call_id + 1;
    return modem->incoming_call_id;
}

int32_t h2_quectel_incoming_call_current(const h2_quectel_modem_t *modem) {
    return modem != NULL ? modem->incoming_call_id : 0;
}

int32_t h2_quectel_incoming_call_end(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return 0;
    }
    const int32_t call_id = modem->incoming_call_id;
    modem->incoming_call_id = 0;
    return call_id;
}

static void post_call_event(
    h2_quectel_modem_t *modem,
    h2_pal_system_event_type_t type,
    const h2_pal_modem_call_status_t *status) {
    if (status == NULL) {
        return;
    }
    h2_pal_modem_call_event_t event;
    memset(&event, 0, sizeof(event));
    event.call = *status;
    h2_quectel_post_system_event(modem, type, &event, sizeof(event));
}

static int dial_number_valid(
    const char *number,
    size_t number_capacity,
    size_t *out_number_len) {
    if (number == NULL || number_capacity == 0u || out_number_len == NULL) {
        return 0;
    }
    for (size_t i = 0u; i < number_capacity; ++i) {
        const unsigned char ch = (unsigned char)number[i];
        if (ch == '\0') {
            *out_number_len = i;
            return i != 0u;
        }
        if (!(h2_quectel_ascii_digit(ch) || ch == '+' || ch == '*' || ch == '#' || ch == ',' || ch == 'p' || ch == 'P' || ch == 'w' || ch == 'W')) {
            return 0;
        }
    }
    return 0;
}

int h2_quectel_parse_clcc_line(const char *line, h2_pal_modem_call_status_t *out_status) {
    if (line == NULL || out_status == NULL) {
        return 0;
    }
    int id = 0;
    int dir = 0;
    int state = 0;
    if (sscanf(line, "+CLCC: %d,%d,%d", &id, &dir, &state) < 3) {
        return 0;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->call_id = id;
    out_status->direction = dir == 0 ? H2_PAL_MODEM_CALL_DIRECTION_OUTGOING : H2_PAL_MODEM_CALL_DIRECTION_INCOMING;
    out_status->state = parse_clcc_state(state);
    const char *quote = strchr(line, '"');
    if (quote != NULL) {
        h2_quectel_copy_token(out_status->number, sizeof(out_status->number), quote);
    }
    return 1;
}

h2_pal_result_t h2_quectel_modem_call_dial(
    h2_pal_modem_t *platform,
    const h2_pal_modem_call_request_t *request) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    size_t number_len = 0u;
    if (modem == NULL || request == NULL ||
        !dial_number_valid(
            request->number, sizeof(request->number), &number_len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    char cmd[H2_QUECTEL_LINE_MAX];
    int n = snprintf(cmd, sizeof(cmd), "ATD%s;", request->number);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, cmd, NULL, 0);
    if (rc == H2_PAL_OK) {
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        status.call_id = -1;
        status.direction = H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
        status.state = H2_PAL_MODEM_CALL_STATE_DIALING;
        memcpy(status.number, request->number, number_len + 1u);
        post_call_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED, &status);
    }
    return rc;
}

h2_pal_result_t h2_quectel_modem_call_answer(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = modem != NULL ? h2_quectel_at_exchange(modem, "ATA", NULL, 0) : H2_PAL_ERR_INVALID_ARG;
    if (rc == H2_PAL_OK) {
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        const int32_t incoming_call_id =
            h2_quectel_incoming_call_current(modem);
        status.call_id = incoming_call_id != 0 ? incoming_call_id : -1;
        status.direction = incoming_call_id != 0
            ? H2_PAL_MODEM_CALL_DIRECTION_INCOMING
            : H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
        status.state = H2_PAL_MODEM_CALL_STATE_ACTIVE;
        post_call_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED, &status);
    }
    return rc;
}

h2_pal_result_t h2_quectel_modem_call_hangup(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = modem != NULL ? h2_quectel_at_exchange(modem, "ATH", NULL, 0) : H2_PAL_ERR_INVALID_ARG;
    if (rc == H2_PAL_OK) {
        h2_pal_modem_call_status_t status;
        memset(&status, 0, sizeof(status));
        const int32_t incoming_call_id =
            h2_quectel_incoming_call_current(modem);
        status.call_id = incoming_call_id != 0 ? incoming_call_id : -1;
        status.direction = incoming_call_id != 0
            ? H2_PAL_MODEM_CALL_DIRECTION_INCOMING
            : H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
        status.state = H2_PAL_MODEM_CALL_STATE_ENDED;
        post_call_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED, &status);
        if (incoming_call_id != 0) {
            (void)h2_quectel_incoming_call_end(modem);
        }
    }
    return rc;
}

h2_pal_result_t h2_quectel_modem_get_call_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_call_status_t *out_status) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->call_id = -1;
    out_status->state = H2_PAL_MODEM_CALL_STATE_IDLE;

    h2_quectel_response_t response;
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+CLCC", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const char *line = h2_quectel_response_find(&response, "+CLCC:");
    if (line == NULL) {
        return H2_PAL_OK;
    }
    if (!h2_quectel_parse_clcc_line(line, out_status)) {
        return H2_PAL_ERR_FORMAT;
    }
    if (out_status->direction == H2_PAL_MODEM_CALL_DIRECTION_INCOMING) {
        out_status->call_id = h2_quectel_incoming_call_begin(modem);
    }
    if (out_status->state == H2_PAL_MODEM_CALL_STATE_INCOMING) {
        post_call_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING, out_status);
    } else {
        post_call_event(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED, out_status);
    }
    return H2_PAL_OK;
}
