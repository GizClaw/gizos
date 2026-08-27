#include "h2_simcom_modem.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct command_state {
    int malformed_imei;
    int unsupported_manufacturer;
    int unsupported_model;
    int sim_absent;
    int sim_not_ready;
    int data_unsupported;
    int call_unsupported;
    int gnss_unsupported;
    int gnss_no_fix;
    int gnss_malformed;
    int gnss_inline_ready;
    int gnss_wait_error;
    int gnss_wait_calls;
    uint32_t gnss_wait_timeout_ms;
    int data_open_error;
    int data_open_calls;
    int data_close_calls;
    int data_close_error;
    int deinit_calls;
    int deinit_error;
    char last_command[64];
    size_t command_calls;
} command_state_t;

static h2_pal_result_t command_response(
    void *user,
    const char *cmd,
    char *response,
    size_t response_size,
    uint32_t timeout_ms) {
    (void)timeout_ms;
    command_state_t *state = (command_state_t *)user;
    assert(strlen(cmd) < sizeof(state->last_command));
    strcpy(state->last_command, cmd);
    state->command_calls++;
    const char *text = "OK\r\n";
    h2_pal_result_t rc = H2_PAL_OK;
    if (strcmp(cmd, "AT+CGMI") == 0) {
        text = state->unsupported_manufacturer
            ? "OTHER VENDOR\r\nOK\r\n"
            : "INCORPORATED\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CGMM") == 0) {
        text = state->unsupported_model
            ? "OTHER-MODEM\r\nOK\r\n"
            : "A7670E-FASE\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CGMR") == 0) {
        text = "A011B01A7670M7\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CGSN") == 0) {
        text = state->malformed_imei
            ? "1234BAD\r\nOK\r\n"
            : "123456789012345\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CIMI") == 0) {
        text = state->sim_absent ? "+CME ERROR: 10\r\n" : "460001234567890\r\nOK\r\n";
        rc = state->sim_absent ? H2_PAL_ERR_IO : H2_PAL_OK;
    } else if (strcmp(cmd, "AT+CPIN?") == 0) {
        text = state->sim_absent
            ? "+CME ERROR: 10\r\n"
            : (state->sim_not_ready
                ? "+CPIN: NOT READY\r\nOK\r\n"
                : "+CPIN: READY\r\nOK\r\n");
        rc = state->sim_absent ? H2_PAL_ERR_IO : H2_PAL_OK;
    } else if (strcmp(cmd, "AT+CEREG?") == 0) {
        text = state->sim_absent ? "+CEREG: 0,0\r\nOK\r\n" : "+CEREG: 0,1\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CGATT?") == 0) {
        text = state->data_unsupported
            ? "ERROR\r\n"
            : (state->sim_absent ? "+CGATT: 0\r\nOK\r\n" : "+CGATT: 1\r\nOK\r\n");
        rc = state->data_unsupported ? H2_PAL_ERR_IO : H2_PAL_OK;
    } else if (strcmp(cmd, "AT+CSQ") == 0) {
        text = "+CSQ: 18,0\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CLCC") == 0) {
        text = state->call_unsupported
            ? "ERROR\r\n"
            : "+CLCC: 1,0,0,0,0,\"10086\",129\r\nOK\r\n";
        rc = state->call_unsupported ? H2_PAL_ERR_IO : H2_PAL_OK;
    } else if (strcmp(cmd, "AT+CGNSSPWR?") == 0) {
        text = state->gnss_unsupported ? "ERROR\r\n" : "+CGNSSPWR: 1,1,1,0\r\nOK\r\n";
        rc = state->gnss_unsupported ? H2_PAL_ERR_IO : H2_PAL_OK;
    } else if (strcmp(cmd, "AT+CGNSSPWR=1") == 0 &&
               state->gnss_inline_ready) {
        text = "+CGNSSPWR: READY!\r\nOK\r\n";
    } else if (strcmp(cmd, "AT+CGPSINFO") == 0) {
        if (state->gnss_no_fix) {
            text = "+CGPSINFO: ,,,,,,,,\r\nOK\r\n";
        } else if (state->gnss_malformed) {
            text = "+CGPSINFO: 9160.0000,N,18100.0000,E,320722,251530.0,12.5,1.5,361.0\r\nOK\r\n";
        } else {
            text = "+CGPSINFO: 3112.3456,N,12128.7654,E,260722,081530.0,12.5,1.5,180.0\r\nOK\r\n";
        }
    }
    assert(strlen(text) + 1u <= response_size);
    strcpy(response, text);
    return rc;
}

static h2_pal_result_t data_open(
    void *user,
    uint32_t timeout_ms,
    h2_pal_modem_data_status_t *out_status) {
    assert(timeout_ms == 1234u);
    command_state_t *state = (command_state_t *)user;
    state->data_open_calls++;
    memset(out_status, 0, sizeof(*out_status));
    if (state->data_open_error) {
        return H2_PAL_ERR_IO;
    }
    out_status->ip4_valid = 1u;
    out_status->ip4 = 0x01020304u;
    return H2_PAL_OK;
}

static h2_pal_result_t data_close(void *user, uint32_t timeout_ms) {
    assert(timeout_ms == 5678u || timeout_ms == 1000u);
    command_state_t *state = (command_state_t *)user;
    state->data_close_calls++;
    return state->data_close_error ? H2_PAL_ERR_TIMEOUT : H2_PAL_OK;
}

static h2_pal_result_t deinit_transport(void *user) {
    command_state_t *state = (command_state_t *)user;
    state->deinit_calls++;
    return state->deinit_error ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t wait_gnss_ready(void *user, uint32_t timeout_ms) {
    command_state_t *state = (command_state_t *)user;
    state->gnss_wait_calls++;
    state->gnss_wait_timeout_ms = timeout_ms;
    return state->gnss_wait_error ? H2_PAL_ERR_TIMEOUT : H2_PAL_OK;
}

static void init_command_modem(
    command_state_t *state,
    h2_simcom_modem_t *modem) {
    const h2_simcom_modem_config_t config = {
        .transport_user = state,
        .deinit = deinit_transport,
        .command = command_response,
        .data_open = data_open,
        .data_close = data_close,
        .wait_gnss_ready = wait_gnss_ready,
    };
    assert(h2_simcom_modem_init(modem, &config) == H2_PAL_OK);
}

static void test_whole_modem_close_skips_graceful_data_close(void) {
    command_state_t state = { 0 };
    h2_simcom_modem_t modem;
    init_command_modem(&state, &modem);
    h2_pal_modem_t *api = h2_simcom_modem_platform(&modem);

    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_data_open(api, 1234u) == H2_PAL_OK);
    assert(h2_pal_modem_close(api, 1u) == H2_PAL_OK);
    assert(state.data_close_calls == 0);
    assert(state.deinit_calls == 1);
    assert(modem.opened == 0u);
    assert(modem.prepared == 0u);
    assert(modem.capabilities == 0u);
    assert(modem.data_status.state == H2_PAL_MODEM_DATA_CLOSED);

    assert(h2_pal_modem_close(api, 1u) == H2_PAL_OK);
    assert(state.deinit_calls == 1);
    h2_simcom_modem_deinit(&modem);
}

static void test_whole_modem_close_failure_is_retryable(void) {
    command_state_t state = { .deinit_error = 1 };
    h2_simcom_modem_t modem;
    init_command_modem(&state, &modem);
    h2_pal_modem_t *api = h2_simcom_modem_platform(&modem);

    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_data_open(api, 1234u) == H2_PAL_OK);
    const uint32_t capabilities = modem.capabilities;
    assert(h2_pal_modem_close(api, 1u) == H2_PAL_ERR_IO);
    assert(state.data_close_calls == 0);
    assert(state.deinit_calls == 1);
    assert(modem.opened != 0u);
    assert(modem.prepared != 0u);
    assert(modem.capabilities == capabilities);
    assert(modem.data_status.state == H2_PAL_MODEM_DATA_OPEN);

    state.deinit_error = 0;
    assert(h2_pal_modem_close(api, 1u) == H2_PAL_OK);
    assert(state.deinit_calls == 2);
    assert(modem.opened == 0u);
    assert(modem.data_status.state == H2_PAL_MODEM_DATA_CLOSED);
    h2_simcom_modem_deinit(&modem);
}

static void test_graceful_data_close_failure_keeps_open_state(void) {
    command_state_t state = { .data_close_error = 1 };
    h2_simcom_modem_t modem;
    init_command_modem(&state, &modem);
    h2_pal_modem_t *api = h2_simcom_modem_platform(&modem);

    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_data_open(api, 1234u) == H2_PAL_OK);
    assert(h2_pal_modem_data_close(api, 5678u) == H2_PAL_ERR_TIMEOUT);
    assert(state.data_close_calls == 1);
    assert(modem.data_status.state == H2_PAL_MODEM_DATA_OPEN);
    assert(modem.data_status.ip4_valid != 0u);
    assert(modem.data_status.last_error == H2_PAL_ERR_TIMEOUT);

    assert(h2_pal_modem_close(api, 1u) == H2_PAL_OK);
    assert(state.data_close_calls == 1);
    assert(state.deinit_calls == 1);
    h2_simcom_modem_deinit(&modem);
}

static void test_identity_status_data_call_and_gnss(void) {
    command_state_t state = { 0 };
    h2_simcom_modem_t modem;
    init_command_modem(&state, &modem);
    h2_pal_modem_t *api = h2_simcom_modem_platform(&modem);
    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);

    uint32_t capabilities = 0u;
    assert(h2_pal_modem_get_capabilities(api, &capabilities) == H2_PAL_OK);
    assert(capabilities == (H2_PAL_MODEM_CAPABILITY_DATA |
                            H2_PAL_MODEM_CAPABILITY_CALL |
                            H2_PAL_MODEM_CAPABILITY_GNSS));

    h2_pal_modem_identity_t identity;
    assert(h2_pal_modem_get_identity(api, &identity) == H2_PAL_OK);
    assert(strcmp(identity.manufacturer, "INCORPORATED") == 0);
    assert(strcmp(identity.model, "A7670E-FASE") == 0);
    assert(strcmp(identity.imei, "123456789012345") == 0);

    h2_pal_modem_status_t status;
    assert(h2_pal_modem_get_status(api, &status) == H2_PAL_OK);
    assert(status.sim == H2_PAL_MODEM_SIM_STATE_READY);
    assert(status.registration == H2_PAL_MODEM_REGISTRATION_HOME);
    assert(status.packet == H2_PAL_MODEM_PACKET_ATTACHED);

    assert(h2_pal_modem_data_open(api, 1234u) == H2_PAL_OK);
    h2_pal_modem_data_status_t data_status;
    assert(h2_pal_modem_get_data_status(api, &data_status) == H2_PAL_OK);
    assert(data_status.state == H2_PAL_MODEM_DATA_OPEN && data_status.ip4_valid != 0u);
    assert(h2_pal_modem_data_close(api, 5678u) == H2_PAL_OK);

    h2_pal_modem_call_status_t call;
    assert(h2_pal_modem_get_call_status(api, &call) == H2_PAL_OK);
    assert(call.state == H2_PAL_MODEM_CALL_STATE_ACTIVE);
    assert(strcmp(call.number, "10086") == 0);

    h2_pal_modem_gnss_state_t gnss_state;
    assert(h2_pal_modem_gnss_start(api, 9000u) == H2_PAL_OK);
    assert(state.gnss_wait_calls == 1);
    assert(state.gnss_wait_timeout_ms == 9000u);
    assert(h2_pal_modem_get_gnss_state(api, &gnss_state) == H2_PAL_OK);
    assert(gnss_state == H2_PAL_MODEM_GNSS_ACQUIRING);
    h2_pal_modem_gnss_fix_t fix;
    assert(h2_pal_modem_get_gnss_fix(api, &fix) == H2_PAL_OK);
    assert(fix.valid != 0u && fix.year == 2022u && fix.month == 7u && fix.day == 26u);
    assert(h2_pal_modem_gnss_stop(api, 1000u) == H2_PAL_OK);

    assert(h2_pal_modem_close(api, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_close(api, 1000u) == H2_PAL_OK);
    h2_simcom_modem_deinit(&modem);
}

static void test_absent_sim_and_bad_imei(void) {
    command_state_t state = { .sim_absent = 1 };
    h2_simcom_modem_t modem;
    init_command_modem(&state, &modem);
    h2_pal_modem_t *api = h2_simcom_modem_platform(&modem);
    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);
    h2_pal_modem_status_t status;
    assert(h2_pal_modem_get_status(api, &status) == H2_PAL_OK);
    assert(status.sim == H2_PAL_MODEM_SIM_STATE_ABSENT);
    assert(h2_pal_modem_close(api, 1000u) == H2_PAL_OK);
    h2_simcom_modem_deinit(&modem);

    state = (command_state_t){ .sim_not_ready = 1 };
    init_command_modem(&state, &modem);
    api = h2_simcom_modem_platform(&modem);
    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_get_status(api, &status) == H2_PAL_OK);
    assert(status.sim == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
    assert(h2_pal_modem_close(api, 1000u) == H2_PAL_OK);
    h2_simcom_modem_deinit(&modem);

    state = (command_state_t){ .malformed_imei = 1 };
    init_command_modem(&state, &modem);
    api = h2_simcom_modem_platform(&modem);
    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_ERR_FORMAT);
    h2_simcom_modem_deinit(&modem);

    state = (command_state_t){ .unsupported_model = 1 };
    init_command_modem(&state, &modem);
    api = h2_simcom_modem_platform(&modem);
    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_ERR_UNSUPPORTED);
    h2_simcom_modem_deinit(&modem);

    state = (command_state_t){ .unsupported_manufacturer = 1 };
    init_command_modem(&state, &modem);
    api = h2_simcom_modem_platform(&modem);
    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_ERR_UNSUPPORTED);
    h2_simcom_modem_deinit(&modem);
}

static void test_capabilities_are_probed(void) {
    command_state_t state = {
        .data_unsupported = 1,
        .call_unsupported = 1,
        .gnss_unsupported = 1,
    };
    h2_simcom_modem_t modem;
    init_command_modem(&state, &modem);
    h2_pal_modem_t *api = h2_simcom_modem_platform(&modem);
    uint32_t capabilities = UINT32_MAX;

    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_get_capabilities(api, &capabilities) == H2_PAL_OK);
    assert(capabilities == 0u);
    assert(h2_pal_modem_close(api, 1000u) == H2_PAL_OK);
    h2_simcom_modem_deinit(&modem);
}

static void test_gnss_no_fix_and_malformed_fix(void) {
    command_state_t state = { .gnss_no_fix = 1 };
    h2_simcom_modem_t modem;
    init_command_modem(&state, &modem);
    h2_pal_modem_t *api = h2_simcom_modem_platform(&modem);
    h2_pal_modem_gnss_fix_t fix;

    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_get_gnss_fix(api, &fix) == H2_PAL_ERR_UNAVAILABLE);
    assert(h2_pal_modem_close(api, 1000u) == H2_PAL_OK);
    h2_simcom_modem_deinit(&modem);

    state = (command_state_t){ .gnss_malformed = 1 };
    init_command_modem(&state, &modem);
    api = h2_simcom_modem_platform(&modem);
    assert(h2_pal_modem_open(api, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_get_gnss_fix(api, &fix) == H2_PAL_ERR_FORMAT);
    assert(h2_pal_modem_close(api, 1000u) == H2_PAL_OK);
    h2_simcom_modem_deinit(&modem);

    state = (command_state_t){ .gnss_wait_error = 1 };
    init_command_modem(&state, &modem);
    api = h2_simcom_modem_platform(&modem);
    assert(h2_pal_modem_gnss_start(api, 123u) == H2_PAL_ERR_TIMEOUT);
    assert(state.gnss_wait_calls == 1);
    assert(state.gnss_wait_timeout_ms == 123u);
    h2_simcom_modem_deinit(&modem);
}

static void test_inline_gnss_ready_without_wait_backend(void) {
    command_state_t state = { .gnss_inline_ready = 1 };
    h2_simcom_modem_t modem;
    const h2_simcom_modem_config_t config = {
        .transport_user = &state,
        .command = command_response,
    };
    assert(h2_simcom_modem_init(&modem, &config) == H2_PAL_OK);
    assert(h2_pal_modem_gnss_start(
               h2_simcom_modem_platform(&modem), 9000u) == H2_PAL_OK);
    h2_simcom_modem_deinit(&modem);

    state = (command_state_t){ 0 };
    assert(h2_simcom_modem_init(&modem, &config) == H2_PAL_OK);
    assert(h2_pal_modem_gnss_start(
               h2_simcom_modem_platform(&modem), 9000u) ==
           H2_PAL_ERR_UNSUPPORTED);
    h2_simcom_modem_deinit(&modem);
}

typedef struct system_event_state {
    h2_pal_system_event_type_t type;
    h2_pal_modem_data_status_t data_status;
    h2_pal_modem_call_event_t call_event;
    h2_pal_modem_event_t modem_event;
    size_t posts;
} system_event_state_t;

static int system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms) {
    system_event_state_t *state = (system_event_state_t *)user;
    assert(timeout_ms == 0u);
    assert(event != NULL);
    state->type = event->type;
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_ERROR) {
        assert(event->payload_size == sizeof(state->modem_event));
        memcpy(&state->modem_event, event->payload, sizeof(state->modem_event));
    } else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED ||
               event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING ||
               event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED) {
        assert(event->payload_size == sizeof(state->call_event));
        memcpy(&state->call_event, event->payload, sizeof(state->call_event));
    } else {
        assert(event->payload_size == sizeof(state->data_status));
        memcpy(&state->data_status, event->payload, sizeof(state->data_status));
    }
    state->posts++;
    return H2_PAL_OK;
}

static void test_dial_number_boundaries(void) {
    static const h2_pal_system_event_vtable_t event_vtable = {
        .post = system_event_post,
    };
    command_state_t command = { 0 };
    system_event_state_t events = { 0 };
    const h2_pal_system_event_api_t event_api = {
        .user = &events,
        .vtable = &event_vtable,
    };
    h2_simcom_modem_t modem;
    const h2_simcom_modem_config_t config = {
        .transport_user = &command,
        .command = command_response,
        .system_events = &event_api,
    };
    assert(h2_simcom_modem_init(&modem, &config) == H2_PAL_OK);

    h2_pal_modem_call_request_t request;
    memset(&request, 0, sizeof(request));
    memset(request.number, '1', sizeof(request.number) - 1u);
    assert(h2_pal_modem_call_dial(
               h2_simcom_modem_platform(&modem), &request) == H2_PAL_OK);
    assert(command.command_calls == 1u);
    assert(command.last_command[0] == 'A');
    assert(strcmp(
               command.last_command,
               "ATD1111111111111111111111111111111;") == 0);
    assert(events.posts == 1u);
    assert(events.type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED);
    assert(events.call_event.call.state == H2_PAL_MODEM_CALL_STATE_DIALING);
    assert(strcmp(events.call_event.call.number, request.number) == 0);

    memset(request.number, '1', sizeof(request.number));
    assert(h2_pal_modem_call_dial(
               h2_simcom_modem_platform(&modem), &request) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(command.command_calls == 1u);
    assert(events.posts == 1u);
    h2_simcom_modem_deinit(&modem);
}

static void test_data_failure_and_unsolicited_close_events(void) {
    static const h2_pal_system_event_vtable_t event_vtable = {
        .post = system_event_post,
    };
    command_state_t command = { .data_open_error = 1 };
    system_event_state_t events = { 0 };
    const h2_pal_system_event_api_t event_api = {
        .user = &events,
        .vtable = &event_vtable,
    };
    h2_simcom_modem_t modem;
    const h2_simcom_modem_config_t config = {
        .transport_user = &command,
        .command = command_response,
        .data_open = data_open,
        .data_close = data_close,
        .system_events = &event_api,
    };
    assert(h2_simcom_modem_init(&modem, &config) == H2_PAL_OK);

    assert(h2_pal_modem_data_open(
               h2_simcom_modem_platform(&modem), 1234u) == H2_PAL_ERR_IO);
    assert(events.posts == 1u);
    assert(events.type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_ERROR);
    assert(events.modem_event.result == H2_PAL_ERR_IO);

    modem.data_status.state = H2_PAL_MODEM_DATA_OPEN;
    modem.data_status.ip4_valid = 1u;
    h2_simcom_modem_notify_data_closed(&modem, H2_PAL_ERR_TIMEOUT);
    assert(events.posts == 2u);
    assert(events.type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_CLOSED);
    assert(events.data_status.state == H2_PAL_MODEM_DATA_CLOSED);
    assert(events.data_status.ip4_valid == 0u);
    assert(events.data_status.last_error == H2_PAL_ERR_TIMEOUT);
    h2_simcom_modem_notify_data_closed(&modem, H2_PAL_ERR_TIMEOUT);
    assert(events.posts == 2u);
    h2_simcom_modem_deinit(&modem);
}

typedef struct raw_state {
    const char *rx;
    size_t rx_offset;
    size_t write_count;
} raw_state_t;

static h2_pal_result_t raw_read(
    void *user,
    uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *out_len) {
    (void)timeout_ms;
    raw_state_t *state = (raw_state_t *)user;
    assert(len >= 1u);
    if (state->rx[state->rx_offset] == '\0') {
        *out_len = 0u;
        return H2_PAL_ERR_TIMEOUT;
    }
    buf[0] = (uint8_t)state->rx[state->rx_offset++];
    *out_len = 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t raw_write(
    void *user,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *out_len) {
    (void)buf;
    (void)timeout_ms;
    ((raw_state_t *)user)->write_count++;
    *out_len = len;
    return H2_PAL_OK;
}

static void test_fragmented_raw_transport_with_urc(void) {
    static const char responses[] =
        "AT\r\nRING\r\nOK\r\n"
        "ATE0\r\nOK\r\n"
        "AT+CMEE=2\r\nOK\r\n"
        "AT+CLIP=1\r\nOK\r\n"
        "AT+CREG=1\r\nOK\r\n"
        "AT+CGREG=1\r\nOK\r\n"
        "AT+CEREG=1\r\nOK\r\n"
        "AT+CGMI\r\n+CEREG: 0,1\r\n+CPIN: READY\r\nSIMCOM INCORPORATED\r\nOK\r\n"
        "AT+CGMM\r\nA7670E-FASE\r\nOK\r\n"
        "AT+CGMR\r\nA011B01A7670M7\r\nOK\r\n"
        "AT+CGSN\r\n123456789012345\r\nOK\r\n"
        "AT+CIMI\r\n+CME ERROR: 10\r\n"
        "AT+CLCC\r\nOK\r\n"
        "AT+CGNSSPWR?\r\n+CGNSSPWR: 1,1,1,0\r\nOK\r\n";
    raw_state_t state = { .rx = responses };
    h2_simcom_modem_t modem;
    const h2_simcom_modem_config_t config = {
        .transport_user = &state,
        .read = raw_read,
        .write = raw_write,
    };
    assert(h2_simcom_modem_init(&modem, &config) == H2_PAL_OK);
    assert(h2_pal_modem_open(h2_simcom_modem_platform(&modem), 1000u) == H2_PAL_OK);
    assert(state.write_count == 14u);
    h2_simcom_modem_deinit(&modem);
}

static void assert_ppp_rejection_is_terminal(const char *final_result) {
    char responses[256];
    const int written = snprintf(
        responses,
        sizeof(responses),
        "OK\r\nOK\r\nOK\r\nOK\r\nOK\r\nOK\r\nOK\r\n"
        "OK\r\nOK\r\n%s\r\n",
        final_result);
    assert(written > 0 && (size_t)written < sizeof(responses));
    raw_state_t state = { .rx = responses };
    h2_simcom_modem_t modem;
    const h2_simcom_modem_config_t config = {
        .transport_user = &state,
        .read = raw_read,
        .write = raw_write,
    };
    assert(h2_simcom_modem_init(&modem, &config) == H2_PAL_OK);
    assert(h2_simcom_modem_dial_ppp(&modem) == H2_PAL_ERR_IO);
    assert(state.write_count == 10u);
    h2_simcom_modem_deinit(&modem);
}

static void test_ppp_rejections_are_terminal(void) {
    assert_ppp_rejection_is_terminal("NO CARRIER");
    assert_ppp_rejection_is_terminal("BUSY");
    assert_ppp_rejection_is_terminal("NO ANSWER");
}

int main(void) {
    test_whole_modem_close_skips_graceful_data_close();
    test_whole_modem_close_failure_is_retryable();
    test_graceful_data_close_failure_keeps_open_state();
    test_identity_status_data_call_and_gnss();
    test_absent_sim_and_bad_imei();
    test_capabilities_are_probed();
    test_gnss_no_fix_and_malformed_fix();
    test_inline_gnss_ready_without_wait_backend();
    test_dial_number_boundaries();
    test_data_failure_and_unsolicited_close_events();
    test_fragmented_raw_transport_with_urc();
    test_ppp_rejections_are_terminal();
    return 0;
}
