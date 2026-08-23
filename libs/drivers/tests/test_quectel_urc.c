#include "h2_quectel_modem.h"

#include <assert.h>
#include <string.h>

typedef struct system_event_record {
    h2_pal_system_event_type_t type;
    h2_pal_modem_call_event_t call;
} system_event_record_t;

typedef struct system_event_state {
    system_event_record_t records[16];
    size_t posts;
} system_event_state_t;

typedef struct command_state {
    char last_command[64];
    size_t calls;
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
    state->calls++;
    static const char ok[] = "OK\r\n";
    assert(response != NULL);
    assert(response_size >= sizeof(ok));
    memcpy(response, ok, sizeof(ok));
    return H2_PAL_OK;
}

static int system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms) {
    system_event_state_t *state = (system_event_state_t *)user;
    assert(timeout_ms == 0u);
    assert(event != NULL);
    assert(event->payload != NULL);
    assert(state->posts < sizeof(state->records) / sizeof(state->records[0]));
    system_event_record_t *record = &state->records[state->posts];
    assert(event->payload_size == sizeof(record->call));
    record->type = event->type;
    memcpy(&record->call, event->payload, sizeof(record->call));
    state->posts++;
    return H2_PAL_OK;
}

int main(void) {
    static const h2_pal_system_event_vtable_t event_vtable = {
        .post = system_event_post,
    };
    system_event_state_t events = {0};
    const h2_pal_system_event_api_t event_api = {
        .user = &events,
        .vtable = &event_vtable,
    };
    command_state_t command = {0};
    h2_quectel_modem_t modem;
    const h2_quectel_modem_config_t config = {
        .transport_user = &command,
        .command = command_response,
        .system_events = &event_api,
    };
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);

    h2_quectel_handle_urc_line(&modem, "RING");
    h2_quectel_handle_urc_line(&modem, "+CRING: VOICE");
    assert(events.posts == 2u);
    const int32_t first_call_id = events.records[0].call.call.call_id;
    assert(first_call_id > 0);
    assert(events.records[0].type ==
           H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING);
    assert(events.records[1].call.call.call_id == first_call_id);
    assert(events.records[1].call.call.number[0] == '\0');

    h2_quectel_handle_urc_line(
        &modem, "+CLIP: \"13800138000\",129,,,,0");
    assert(events.posts == 3u);
    assert(events.records[2].type ==
           H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING);
    assert(events.records[2].call.call.call_id == first_call_id);
    assert(events.records[2].call.call.direction ==
           H2_PAL_MODEM_CALL_DIRECTION_INCOMING);
    assert(events.records[2].call.call.state ==
           H2_PAL_MODEM_CALL_STATE_INCOMING);
    assert(strcmp(events.records[2].call.call.number, "13800138000") == 0);

    h2_quectel_handle_urc_line(
        &modem, "+CLCC: 7,1,4,0,0,\"13800138000\",129");
    assert(events.posts == 4u);
    assert(events.records[3].type ==
           H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING);
    assert(events.records[3].call.call.call_id == first_call_id);
    assert(strcmp(events.records[3].call.call.number, "13800138000") == 0);

    h2_quectel_handle_urc_line(&modem, "NO CARRIER");
    assert(events.posts == 5u);
    assert(events.records[4].type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED);
    assert(events.records[4].call.call.call_id == first_call_id);

    h2_quectel_handle_urc_line(
        &modem, "+CLIP: \"13800138000\",129,,,,0");
    assert(events.posts == 5u);
    h2_quectel_handle_urc_line(
        &modem, "+CLCC: 7,1,4,0,0,\"13800138000\",129");
    assert(events.posts == 5u);

    h2_quectel_handle_urc_line(&modem, "RING");
    assert(events.posts == 6u);
    const int32_t second_call_id = events.records[5].call.call.call_id;
    assert(second_call_id > 0);
    assert(second_call_id != first_call_id);
    h2_quectel_handle_urc_line(
        &modem, "+CLCC: 7,1,4,0,0,\"13900139000\",129");
    assert(events.posts == 7u);
    assert(events.records[6].call.call.call_id == second_call_id);
    assert(strcmp(events.records[6].call.call.number, "13900139000") == 0);

    h2_pal_modem_t *platform = h2_quectel_modem_platform(&modem);
    assert(h2_pal_modem_call_answer(platform, 1000u) == H2_PAL_OK);
    assert(events.posts == 8u);
    assert(events.records[7].type ==
           H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED);
    assert(events.records[7].call.call.call_id == second_call_id);
    assert(events.records[7].call.call.state == H2_PAL_MODEM_CALL_STATE_ACTIVE);

    assert(h2_pal_modem_call_hangup(platform, 1000u) == H2_PAL_OK);
    assert(events.posts == 9u);
    assert(events.records[8].type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED);
    assert(events.records[8].call.call.call_id == second_call_id);

    h2_quectel_handle_urc_line(
        &modem, "+CLIP: \"13900139000\",129,,,,0");
    assert(events.posts == 9u);

    h2_pal_modem_call_request_t request;
    memset(&request, 0, sizeof(request));
    memset(request.number, '1', sizeof(request.number) - 1u);
    const size_t calls_before_dial = command.calls;
    assert(h2_pal_modem_call_dial(platform, &request) == H2_PAL_OK);
    assert(command.calls == calls_before_dial + 1u);
    assert(strcmp(
               command.last_command,
               "ATD1111111111111111111111111111111;") == 0);
    assert(events.posts == 10u);
    assert(events.records[9].type ==
           H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED);
    assert(events.records[9].call.call.state ==
           H2_PAL_MODEM_CALL_STATE_DIALING);
    assert(strcmp(events.records[9].call.call.number, request.number) == 0);

    memset(request.number, '1', sizeof(request.number));
    assert(h2_pal_modem_call_dial(platform, &request) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(command.calls == calls_before_dial + 1u);
    assert(events.posts == 10u);

    h2_quectel_modem_deinit(&modem);
    return 0;
}
