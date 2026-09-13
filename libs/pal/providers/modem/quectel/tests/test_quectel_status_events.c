#include "h2_quectel_modem.h"

#include <assert.h>
#include <string.h>

static unsigned registration_events;
static unsigned packet_events;
static unsigned signal_events;
static unsigned call_events;
static unsigned sim_absent_events;

static int post(void *user, const h2_pal_system_event_t *event, uint32_t timeout_ms) {
    (void)user;
    assert(timeout_ms == 0u);
    switch (event->type) {
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED: registration_events++; break;
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED: packet_events++; break;
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED: signal_events++; break;
        case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING: call_events++; break;
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
    test_sim_absent_rx(&events);
    return 0;
}
