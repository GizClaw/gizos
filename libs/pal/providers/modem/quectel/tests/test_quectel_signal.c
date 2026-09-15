#include "h2_quectel_modem.h"

#include <assert.h>
#include <string.h>

static const char *csq_reply = "+CSQ: 20,99\r\nOK\r\n";
static const char *quality_reply;
static h2_pal_result_t quality_result;
static h2_pal_modem_signal_t last_signal;
static unsigned signal_events;
static unsigned exchanges;

static h2_pal_result_t command(void *user, const char *cmd, char *response,
    size_t response_size, uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    const char *text;
    if (strcmp(cmd, "AT+CSQ") == 0) {
        assert(exchanges++ == 0u);
        text = csq_reply;
    } else {
        assert(strcmp(cmd, "AT+QCSQ") == 0);
        assert(exchanges++ == 1u);
        if (quality_result != H2_PAL_OK) {
            return quality_result;
        }
        text = quality_reply;
    }
    assert(strlen(text) < response_size);
    memcpy(response, text, strlen(text) + 1u);
    return H2_PAL_OK;
}

static int post(void *user, const h2_pal_system_event_t *event, uint32_t timeout_ms) {
    (void)user;
    assert(timeout_ms == 0u);
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED) {
        assert(event->payload_size == sizeof(last_signal));
        memcpy(&last_signal, event->payload, sizeof(last_signal));
        signal_events++;
    }
    return H2_PAL_OK;
}

static void check_signal(h2_quectel_modem_t *modem, int rssi, int rsrp) {
    h2_pal_modem_signal_t signal;
    memset(&signal, 0xff, sizeof(signal));
    exchanges = 0u;
    assert(h2_pal_modem_get_signal(&modem->platform, &signal) == H2_PAL_OK);
    assert(exchanges == 2u);
    assert(signal.rssi_dbm == rssi && signal.rssi_valid == (rssi != 0));
    assert(signal.rsrp_dbm == rsrp && signal.rsrp_valid == (rsrp != 0));
    assert(signal.ber == 99 && signal.rat == H2_PAL_MODEM_RAT_LTE);
    assert(last_signal.rssi_dbm == signal.rssi_dbm);
    assert(last_signal.rssi_valid == signal.rssi_valid);
    assert(last_signal.rsrp_dbm == signal.rsrp_dbm);
    assert(last_signal.rsrp_valid == signal.rsrp_valid);
}

int main(void) {
    const h2_pal_system_event_vtable_t vtable = {.post = post};
    const h2_pal_system_event_api_t events = {.vtable = &vtable};
    const h2_quectel_modem_config_t config = {.command = command, .system_events = &events};
    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    static const struct {
        const char *reply;
        int rsrp;
    } cases[] = {
        {"+QCSQ: \"LTE\",-73,-96,10,-12\r\nOK\r\n", -96},
        {"+QCSQ: \"LTE\",-73,-95,10,-12\r\nOK\r\n", -95},
        {"+QCSQ: \"NOSERVICE\"\r\nOK\r\n", 0},
        {"+QCSQ: \"GSM\",-73,-96,10,-12\r\nOK\r\n", 0},
        {"+QCSQ: \"LTE\",-73,-157,10,-12\r\nOK\r\n", 0},
        {"+QCSQ: \"LTE\",-73,-30,10,-12\r\nOK\r\n", 0},
        {"+QCSQ: \"LTE\",-73,-156,10,-12\r\nOK\r\n", -156},
        {"+QCSQ: \"LTE\",-73,-31,10,-12\r\nOK\r\n", -31},
        {"+QCSQ: \"LTE\",-73,-96\r\nOK\r\n", 0},
        {"ERROR\r\n", 0},
        {"OK\r\n", 0},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        quality_reply = cases[i].reply;
        check_signal(&modem, -73, cases[i].rsrp);
        if (i < 2u) {
            assert(signal_events == i + 1u);
        }
    }
    quality_result = H2_PAL_ERR_UNAVAILABLE;
    check_signal(&modem, -73, 0);
    quality_result = H2_PAL_ERR_TIMEOUT;
    check_signal(&modem, -73, 0);
    quality_result = H2_PAL_OK;
    csq_reply = "+CSQ: 99,99\r\nOK\r\n";
    check_signal(&modem, 0, 0);
    quality_result = H2_PAL_ERR_TIMEOUT;
    csq_reply = "+CSQ: 31,99\r\nOK\r\n";
    check_signal(&modem, -51, 0);
    csq_reply = "+CSQ: 32,99\r\nOK\r\n";
    check_signal(&modem, 0, 0);
    csq_reply = "+CSQ: -1,99\r\nOK\r\n";
    check_signal(&modem, 0, 0);
    quality_result = H2_PAL_OK;
    h2_quectel_handle_urc_line(&modem, "+CSQ: 32,99");
    assert(last_signal.rssi_dbm == 0 && last_signal.rssi_valid == 0u);
    h2_quectel_handle_urc_line(&modem, "+CSQ: 20,99");
    assert(last_signal.rssi_valid == 1u && last_signal.rsrp_valid == 0u);
    h2_quectel_handle_urc_line(&modem, "+CSQ: 99,99");
    assert(last_signal.rssi_dbm == 0 && last_signal.rssi_valid == 0u);
    assert(last_signal.rsrp_dbm == 0 && last_signal.rsrp_valid == 0u);
    assert(h2_quectel_modem_deinit(&modem) == H2_PAL_OK);
    return 0;
}
