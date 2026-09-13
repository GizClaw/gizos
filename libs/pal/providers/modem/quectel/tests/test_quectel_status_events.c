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

static h2_pal_result_t failing_cpin_command(void *user, const char *cmd, char *response,
    size_t response_size, uint32_t timeout_ms) {
    if (strcmp(cmd, "AT+CPIN?") == 0) {
        unsigned *calls = user;
        (*calls)++;
        assert(response_size > 0u);
        response[0] = '\0';
        return H2_PAL_ERR_UNAVAILABLE;
    }
    return command(NULL, cmd, response, response_size, timeout_ms);
}

static void test_sim_absent_rx(const h2_pal_system_event_api_t *events) {
    static const char *const absent_lines[] = {
        "+CME ERROR: 10", "+CME ERROR: SIM not inserted",
    };
    for (size_t i = 0u; i < sizeof(absent_lines) / sizeof(absent_lines[0]); i++) {
        unsigned cpin_calls = 0u;
        const h2_quectel_modem_config_t config = {
            .command = failing_cpin_command, .transport_user = &cpin_calls,
            .system_events = events,
        };
        h2_quectel_modem_t modem;
        assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
        h2_pal_modem_status_t status;
        assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
        assert(status.sim == H2_PAL_MODEM_SIM_STATE_UNKNOWN);

        char raw[64];
        const size_t length = strlen(absent_lines[i]);
        assert(length + 3u <= sizeof(raw));
        memcpy(raw, absent_lines[i], length);
        memcpy(raw + length, "\r\n", 3u);
        h2_modem_rx_t receiver = {0};
        /* Without a worker, INVALID_STATE proves the RX tap attempted to post
         * and the post filter accepted the SIM-absent answer. */
        assert(h2_quectel_rx_feed(&modem, &receiver, 0u, (const uint8_t *)raw,
            length + 2u, "AT+CPIN?") == H2_PAL_ERR_INVALID_STATE);
        assert(h2_quectel_post_urc_line(&modem, absent_lines[i]) == H2_PAL_ERR_INVALID_STATE);
        assert(h2_quectel_rx_feed(&modem, &receiver, receiver.next_offset,
            (const uint8_t *)raw, length + 2u, "AT+CSQ") == H2_PAL_OK);
        assert(h2_quectel_rx_feed(&modem, &receiver, receiver.next_offset,
            (const uint8_t *)raw, length + 2u, NULL) == H2_PAL_OK);
        assert(h2_quectel_rx_feed(&modem, &receiver, receiver.next_offset,
            (const uint8_t *)raw, length + 2u, "AT+CPIN?extra") == H2_PAL_OK);
        static const char other_error[] = "+CME ERROR: 100\r\n";
        assert(h2_quectel_rx_feed(&modem, &receiver, receiver.next_offset,
            (const uint8_t *)other_error, sizeof(other_error) - 1u, "AT+CPIN?") == H2_PAL_OK);

        const unsigned before = sim_absent_events;
        h2_quectel_handle_urc_line(&modem, absent_lines[i]);
        assert(sim_absent_events == before + 1u);
        for (unsigned poll = 0u; poll < 2u; poll++) {
            assert(h2_pal_modem_get_status(&modem.platform, &status) == H2_PAL_OK);
            assert(status.sim == H2_PAL_MODEM_SIM_STATE_ABSENT);
        }
        assert(cpin_calls == 3u);
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
