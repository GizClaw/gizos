#include "h2_quectel_modem.h"

#include <assert.h>
#include <string.h>

static unsigned registration_events;
static unsigned packet_events;
static unsigned signal_events;
static unsigned call_events;
static unsigned sim_absent_events;
static unsigned ended_events;
static unsigned changed_events;
static h2_pal_modem_call_status_t last_call;

static int post(void *user, const h2_pal_system_event_t *event, uint32_t timeout_ms) {
    (void)user;
    assert(timeout_ms == 0u);
    switch (event->type) {
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED: registration_events++; break;
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED: packet_events++; break;
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED: signal_events++; break;
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING:
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED:
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED:
            last_call = ((const h2_pal_modem_call_event_t *)event->payload)->call;
            if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING) { call_events++; }
            if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED) { ended_events++; }
            if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED) { changed_events++; }
            break;
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIM_CHANGED: {
            assert(event->payload_size == sizeof(h2_pal_modem_status_t));
            const h2_pal_modem_status_t *status = event->payload;
            if (status->sim == H2_PAL_MODEM_SIM_STATE_ABSENT) { sim_absent_events++; }
            break;
        }
        default: break;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t command(void *user, const char *cmd, char *response,
    size_t response_size, uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    const char *text = "OK\r\n";
    if (strcmp(cmd, "AT+CPIN?") == 0) {
        text = "+CPIN: READY\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CEREG?") == 0) {
        text = "+CEREG: 2,1\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CGATT?") == 0) {
        text = "+CGATT: 1\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CSQ") == 0) {
        text = "+CSQ: 20,99\r\nOK\r\n";
    }
    assert(response_size > strlen(text));
    memcpy(response, text, strlen(text) + 1u);
    return H2_PAL_OK;
}

typedef struct cpin_transport {
    h2_quectel_modem_t *modem;
    h2_modem_rx_t receiver;
    const char *line;
    const char *rx_command;
    unsigned calls;
    int ready;
} cpin_transport_t;

static h2_pal_result_t failing_cpin_command(void *user, const char *cmd, char *response,
    size_t response_size, uint32_t timeout_ms) {
    cpin_transport_t *transport = user;
    if (strcmp(cmd, "AT+CPIN?") == 0) {
        transport->calls++;
        if (transport->ready) {
            return command(NULL, cmd, response, response_size, timeout_ms);
        }
        /* Physical RX sees the terminal line before command() drops its text.
         * No worker is installed: the same poll must consume the RX outcome. */
        if (transport->line != NULL) {
            assert(h2_quectel_rx_feed(transport->modem, &transport->receiver,
                transport->receiver.next_offset, (const uint8_t *)transport->line,
                strlen(transport->line), transport->rx_command) == H2_PAL_OK);
        }
        assert(response_size > 0u);
        response[0] = '\0';
        return H2_PAL_ERR_UNAVAILABLE;
    }
    return command(NULL, cmd, response, response_size, timeout_ms);
}

static void test_sim_absent_rx(const h2_pal_system_event_api_t *events) {
    static const char *const absent_lines[] = {
        "+CME ERROR: 10\r\n", "+CME ERROR: SIM not inserted\r\n",
    };
    for (size_t i = 0u; i < sizeof(absent_lines) / sizeof(absent_lines[0]); i++) {
        h2_quectel_modem_t modem;
        cpin_transport_t transport = {
            .modem = &modem, .line = absent_lines[i], .rx_command = "AT+CPIN?",
        };
        const h2_quectel_modem_config_t config = {
            .command = failing_cpin_command, .transport_user = &transport,
            .system_events = events,
        };
        assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
        h2_pal_modem_status_t status;
        const unsigned before = sim_absent_events;
        assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
        assert(status.sim == H2_PAL_MODEM_SIM_STATE_ABSENT);
        assert(transport.calls == 1u);
        assert(sim_absent_events == before + 1u);
        assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
        assert(status.sim == H2_PAL_MODEM_SIM_STATE_ABSENT);
        assert(sim_absent_events == before + 1u);

        transport.ready = 1;
        assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
        assert(status.sim == H2_PAL_MODEM_SIM_STATE_READY);
        /* A later empty failure must not reuse the earlier absence marker. */
        transport.ready = 0;
        transport.line = NULL;
        assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
        assert(status.sim == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
        assert(sim_absent_events == before + 1u);

        static const char *const other_commands[] = {"AT+CSQ", NULL, "AT+CPIN?extra"};
        for (size_t j = 0u; j < sizeof(other_commands) / sizeof(other_commands[0]); j++) {
            transport.ready = 1;
            assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
            assert(status.sim == H2_PAL_MODEM_SIM_STATE_READY);
            assert(h2_quectel_rx_feed(&modem, &transport.receiver, transport.receiver.next_offset,
                (const uint8_t *)absent_lines[i], strlen(absent_lines[i]), other_commands[j]) == H2_PAL_OK);
            assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_READY);
            transport.ready = 0;
            transport.line = absent_lines[i];
            transport.rx_command = other_commands[j];
            assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
            assert(status.sim == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
            assert(sim_absent_events == before + 1u);
        }
        transport.line = "+CME ERROR: 100\r\n";
        transport.rx_command = "AT+CPIN?";
        assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
        assert(status.sim == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
        assert(sim_absent_events == before + 1u);
        assert(h2_quectel_modem_deinit(&modem) == H2_PAL_OK);
    }
}

/* Exercise the same deferred entry installed on the URC worker deterministically. */
void h2_quectel_sim_recover(void *user);
typedef struct recovery_transport {
    h2_quectel_modem_t *modem;
    const char *cpin;
    unsigned queries;
    unsigned imsi;
    int remove_during_query;
} recovery_transport_t;

static h2_pal_result_t recovery_command(void *user, const char *cmd, char *response,
    size_t size, uint32_t timeout_ms) {
    recovery_transport_t *transport = user;
    assert(timeout_ms == 1000u);
    if (strcmp(cmd, "AT+CPIN?") == 0) {
        transport->queries++;
        if (transport->remove_during_query) {
            h2_quectel_handle_urc_line(transport->modem, "+QSIMSTAT: 1,0");
        }
        assert(size > strlen(transport->cpin));
        strcpy(response, transport->cpin);
        return H2_PAL_OK;
    }
    if (strcmp(cmd, "AT+CIMI") == 0) { transport->imsi++; }
    return command(NULL, cmd, response, size, timeout_ms);
}

static void test_insertion_recovery(const h2_pal_system_event_api_t *events) {
    h2_quectel_modem_t modem;
    recovery_transport_t transport = {.modem = &modem};
    const h2_quectel_modem_config_t config = {
        .command = recovery_command, .transport_user = &transport, .system_events = events,
    };
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    for (unsigned cycle = 0u; cycle < 6u; cycle++) {
        h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,0");
        assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_ABSENT);
        unsigned before = transport.queries;
        h2_quectel_sim_recover(&modem);
        h2_quectel_handle_urc_line(&modem, "+CPIN: READY");
        assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_ABSENT);
        assert(transport.queries == before);
        h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,1");
        assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
        assert(transport.queries == before); /* Callback performs no AT I/O. */
        transport.cpin = "+CPIN: NOT READY\r\nOK\r\n";
        h2_quectel_sim_recover(&modem);
        assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
        if (cycle % 3u == 0u) {
            transport.cpin = "+CPIN: READY\r\nOK\r\n";
            h2_quectel_sim_recover(&modem);
        } else if (cycle % 3u == 1u) {
            h2_quectel_handle_urc_line(&modem, "+CPIN: READY");
        } else {
            transport.cpin = "+CPIN: SIM PIN\r\nOK\r\n";
            h2_quectel_sim_recover(&modem);
            assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_LOCKED);
            assert(modem.sim_poll_remaining == 0u);
            continue;
        }
        assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_READY);
        unsigned imsi_before = transport.imsi;
        for (unsigned tick = 0u; tick < 4u; tick++) { h2_quectel_sim_recover(&modem); }
        assert(transport.imsi == imsi_before + 1u);
        assert(modem.observed_status.registration == H2_PAL_MODEM_REGISTRATION_HOME);
        assert(modem.observed_status.packet == H2_PAL_MODEM_PACKET_ATTACHED);
        assert(modem.sim_poll_remaining == 0u);
    }
    h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,0");
    h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,1");
    transport.cpin = "+CPIN: NOT READY\r\nOK\r\n";
    unsigned before = transport.queries;
    for (unsigned tick = 0u; tick < 40u; tick++) {
        h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,1");
        h2_quectel_sim_recover(&modem);
    }
    assert(transport.queries == before + 30u);
    assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
    h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,0");
    h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,1");
    transport.cpin = "+CPIN: READY\r\nOK\r\n";
    transport.remove_during_query = 1;
    h2_quectel_sim_recover(&modem);
    assert(modem.sim_state == H2_PAL_MODEM_SIM_STATE_ABSENT);
    assert(modem.sim_poll_remaining == 0u);
    assert(h2_quectel_modem_deinit(&modem) == H2_PAL_OK);
}

typedef struct call_transport {
    h2_quectel_modem_t *modem;
    const char *during_command;
} call_transport_t;

static h2_pal_result_t call_command(void *user, const char *cmd, char *response,
    size_t size, uint32_t timeout_ms) {
    call_transport_t *transport = user;
    if (transport->during_command != NULL) {
        h2_quectel_handle_urc_line(transport->modem, transport->during_command);
    }
    if (strcmp(cmd, "AT+CLCC") == 0) {
        const char *text = "+CLCC: 1,1,0,0,0,\"123\",129\r\nOK\r\n";
        assert(size > strlen(text));
        strcpy(response, text);
        return H2_PAL_OK;
    }
    return command(NULL, cmd, response, size, timeout_ms);
}

static void test_dsci_during_command(const h2_pal_system_event_api_t *events) {
    const char *end = "^DSCI: 1,1,6,0,123,129";
    for (unsigned mode = 0u; mode < 3u; mode++) {
        h2_quectel_modem_t modem;
        call_transport_t transport = {.modem = &modem, .during_command = end};
        const h2_quectel_modem_config_t config = {
            .command = call_command, .transport_user = &transport, .system_events = events,
        };
        assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
        h2_quectel_handle_urc_line(&modem, "^DSCI: 1,1,4,0,123,129");
        const int32_t id = last_call.call_id;
        unsigned ended = ended_events;
        if (mode == 0u) {
            assert(h2_pal_modem_call_answer(&modem.platform, 1000u) == H2_PAL_OK);
        } else if (mode == 1u) {
            assert(h2_pal_modem_call_hangup(&modem.platform, 1000u) == H2_PAL_OK);
        } else {
            h2_pal_modem_call_status_t status;
            assert(h2_pal_modem_get_call_status(&modem.platform, &status) == H2_PAL_OK);
            assert(status.state == H2_PAL_MODEM_CALL_STATE_ENDED && status.call_id == id);
        }
        assert(ended_events == ended + 1u && last_call.state == H2_PAL_MODEM_CALL_STATE_ENDED);
        assert(last_call.call_id == id && !modem.call_hold);
        assert(h2_quectel_modem_deinit(&modem) == H2_PAL_OK);
    }
}

/* Both solicited and idle RX must classify DSCI independently of commands. */
int h2_quectel_is_urc(const char *line, const char *command);

static void test_dsci(const h2_pal_system_event_api_t *events) {
    h2_quectel_modem_t modem;
    const h2_quectel_modem_config_t config = {.command = command, .system_events = events};
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    unsigned incoming = call_events, ended = ended_events, changed = changed_events;
    assert(h2_quectel_is_urc("^DSCI: 1,1,4,0,123,129", "AT^DSCI=1"));
    assert(h2_quectel_is_urc("^DSCI: 1,1,4,0,123,129", "AT+CLCC"));
    assert(h2_quectel_is_urc("^DSCI: 1,1,4,0,123,129", NULL));
    h2_quectel_handle_urc_line(&modem, "^DSCI: 1,1,4,1,123,129");
    h2_quectel_handle_urc_line(&modem, "^DSCI: 1,1,6,1,123,129");
    h2_quectel_handle_urc_line(&modem, "^DSCI: 1,1,4,0");
    assert(call_events == incoming && ended_events == ended && !modem.call_hold);
    h2_quectel_handle_urc_line(&modem, "RING");
    int32_t first = last_call.call_id;
    h2_quectel_handle_urc_line(&modem, "+CLIP: \"123\",129");
    h2_quectel_handle_urc_line(&modem, "^DSCI: 1,1,4,0,123,129");
    assert(last_call.call_id == first && call_events == incoming + 2u);
    h2_quectel_handle_urc_line(&modem, "+CLCC: 1,1,4,0,0,\"123\",129");
    assert(call_events == incoming + 2u);
    h2_quectel_handle_urc_line(&modem, "^DSCI: 1,1,6,0,123,129");
    assert(ended_events == ++ended && last_call.call_id == first && !modem.call_hold);
    h2_quectel_handle_urc_line(&modem, "NO CARRIER");
    h2_quectel_handle_urc_line(&modem, "^DSCI: 1,1,6,0,123,129");
    assert(ended_events == ended);
    h2_quectel_handle_urc_line(&modem, "RING");
    int32_t ringing_id = last_call.call_id;
    h2_quectel_handle_urc_line(&modem, "NO CARRIER");
    assert(ended_events == ended && modem.incoming_call_id == ringing_id && modem.call_hold);
    h2_quectel_handle_urc_line(&modem, "^DSCI: 2,1,4,0,\"456\",129");
    int32_t second = last_call.call_id;
    assert(second == ringing_id && second != first && strcmp(last_call.number, "456") == 0);
    h2_quectel_handle_urc_line(&modem, "NO CARRIER");
    h2_quectel_handle_urc_line(&modem, "^DSCI: 1,1,6,0,123,129");
    assert(ended_events == ended && modem.incoming_call_id == second && modem.call_hold);
    assert(h2_pal_modem_call_answer(&modem.platform, 1000u) == H2_PAL_OK);
    assert(last_call.state == H2_PAL_MODEM_CALL_STATE_ACTIVE);
    h2_quectel_handle_urc_line(&modem, "^DSCI: 2,1,3,0,456,129");
    h2_quectel_handle_urc_line(&modem, "+CLCC: 2,1,0,0,0,\"456\",129");
    assert(changed_events == changed + 1u);
    assert(h2_pal_modem_call_hangup(&modem.platform, 1000u) == H2_PAL_OK);
    assert(ended_events == ++ended && last_call.call_id == second);
    h2_quectel_handle_urc_line(&modem, "^DSCI: 2,1,6,0,456,129");
    h2_quectel_handle_urc_line(&modem, "NO CARRIER");
    assert(ended_events == ended);
    h2_quectel_handle_urc_line(&modem, "^DSCI: 3,0,2,0,789,129");
    assert(last_call.state == H2_PAL_MODEM_CALL_STATE_DIALING);
    h2_quectel_handle_urc_line(&modem, "^DSCI: 3,0,7,0,789,129");
    assert(last_call.state == H2_PAL_MODEM_CALL_STATE_ALERTING);
    h2_quectel_handle_urc_line(&modem, "^DSCI: 3,0,3,0,789,129");
    assert(last_call.state == H2_PAL_MODEM_CALL_STATE_ACTIVE);
    h2_quectel_handle_urc_line(&modem, "^DSCI: 3,0,6,0,789,129");
    assert(ended_events == ++ended && last_call.call_id == 3);
    h2_quectel_handle_urc_line(&modem, "RDY");
    h2_quectel_handle_urc_line(&modem, "RING");
    h2_quectel_handle_urc_line(&modem, "NO CARRIER");
    assert(ended_events == ++ended);
    h2_quectel_handle_urc_line(&modem, "NO CARRIER");
    assert(ended_events == ended);
    assert(h2_quectel_modem_deinit(&modem) == H2_PAL_OK);
}

int main(void) {
    const h2_pal_system_event_vtable_t vtable = {.post = post};
    const h2_pal_system_event_api_t events = {.vtable = &vtable};
    const h2_quectel_modem_config_t config = {.command = command, .system_events = &events};
    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    for (unsigned iteration = 0u; iteration < 1000u; iteration++) {
        h2_quectel_handle_urc_line(&modem, "+CREG: 1");
        h2_quectel_handle_urc_line(&modem, "+CGATT: 1");
        h2_quectel_handle_urc_line(&modem, "+CSQ: 20,99");
    }
    assert(registration_events == 1u && packet_events == 1u && signal_events == 1u);
    h2_quectel_handle_urc_line(&modem, "+CREG: 2");
    h2_quectel_handle_urc_line(&modem, "+CREG: 1");
    h2_quectel_handle_urc_line(&modem, "+CGATT: 0");
    h2_quectel_handle_urc_line(&modem, "+CGATT: 1");
    h2_quectel_handle_urc_line(&modem, "+CSQ: 21,99");
    h2_quectel_handle_urc_line(&modem, "+CSQ: 20,99");
    assert(registration_events == 3u && packet_events == 3u && signal_events == 3u);
    for (unsigned iteration = 0u; iteration < 1000u; iteration++) {
        h2_pal_modem_status_t status;
        h2_pal_modem_signal_t signal;
        assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
        assert(h2_pal_modem_get_signal(&modem.platform, &signal) == H2_PAL_OK);
        /* CR/LF-terminated command-mode replies reach the parsers. */
        assert(status.registration == H2_PAL_MODEM_REGISTRATION_HOME);
        assert(status.packet == H2_PAL_MODEM_PACKET_ATTACHED);
        assert(signal.rssi_dbm == -73);
    }
    assert(registration_events == 3u && packet_events == 3u && signal_events == 3u);
    h2_quectel_handle_urc_line(&modem, "RING");
    h2_quectel_handle_urc_line(&modem, "RING");
    assert(call_events == 2u);
    assert(h2_quectel_modem_deinit(&modem) == H2_PAL_OK);
    test_dsci(&events);
    test_dsci_during_command(&events);
    test_sim_absent_rx(&events);
    test_insertion_recovery(&events);
    return 0;
}
