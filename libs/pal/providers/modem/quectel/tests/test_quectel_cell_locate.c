#include "h2_quectel_modem.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Fake, never a real credential: the repository must not carry an
 * integrator token. */
#define FAKE_TOKEN "FAKE0000TESTTOKEN"

#define MAX_RECORDS 32u

typedef struct command_record {
    char cmd[H2_QUECTEL_LINE_MAX];
    uint32_t timeout_ms;
} command_record_t;

typedef struct transport_state {
    command_record_t records[MAX_RECORDS];
    size_t count;
    const char *qlbs_response;
    h2_pal_result_t qlbs_result;
    const char *qlbscfg_response;
    h2_pal_result_t qlbscfg_result;
} transport_state_t;

static h2_pal_result_t transport_command(
    void *user,
    const char *cmd,
    char *response,
    size_t response_size,
    uint32_t timeout_ms) {
    transport_state_t *state = (transport_state_t *)user;
    assert(state->count < MAX_RECORDS);
    assert(strlen(cmd) < H2_QUECTEL_LINE_MAX);
    strcpy(state->records[state->count].cmd, cmd);
    state->records[state->count].timeout_ms = timeout_ms;
    state->count++;

    const char *text = "OK\r\n";
    h2_pal_result_t rc = H2_PAL_OK;
    if (strncmp(cmd, "AT+QLBSCFG=", 11) == 0 && state->qlbscfg_response != NULL) {
        text = state->qlbscfg_response;
        rc = state->qlbscfg_result;
    } else if (strcmp(cmd, "AT+QLBS") == 0) {
        assert(state->qlbs_response != NULL);
        text = state->qlbs_response;
        rc = state->qlbs_result;
    }
    assert(response != NULL);
    assert(response_size > strlen(text));
    memcpy(response, text, strlen(text) + 1u);
    return rc;
}

static size_t count_commands_with(const transport_state_t *state, const char *needle) {
    size_t found = 0u;
    for (size_t i = 0u; i < state->count; ++i) {
        if (strstr(state->records[i].cmd, needle) != NULL) {
            found++;
        }
    }
    return found;
}

static const command_record_t *find_command(const transport_state_t *state, const char *needle) {
    for (size_t i = 0u; i < state->count; ++i) {
        if (strstr(state->records[i].cmd, needle) != NULL) {
            return &state->records[i];
        }
    }
    return NULL;
}

static int bytes_contain(const void *buffer, size_t size, const char *needle) {
    const char *bytes = (const char *)buffer;
    size_t needle_len = strlen(needle);
    if (needle_len == 0u || size < needle_len) {
        return 0;
    }
    for (size_t i = 0u; i + needle_len <= size; ++i) {
        if (memcmp(bytes + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static void init_config(h2_quectel_modem_config_t *config, transport_state_t *state) {
    memset(config, 0, sizeof(*config));
    config->transport_user = state;
    config->command = transport_command;
}

static void test_locate_sends_token_once(void) {
    transport_state_t state;
    memset(&state, 0, sizeof(state));
    state.qlbs_response = "+QLBS: 0,117.200134,31.847649,120\r\nOK\r\n";
    state.qlbs_result = H2_PAL_OK;

    h2_quectel_modem_config_t config;
    init_config(&config, &state);
    config.cell_locate_token = FAKE_TOKEN;
    config.cell_locate_timeout_ms = 45000u;

    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    h2_pal_modem_t *platform = h2_quectel_modem_platform(&modem);

    uint32_t capabilities = 0u;
    assert(h2_pal_modem_get_capabilities(platform, &capabilities) == H2_PAL_OK);
    assert((capabilities & H2_PAL_MODEM_CAPABILITY_CELL_LOCATE) != 0u);

    h2_pal_modem_cell_location_t location;
    memset(&location, 0xAA, sizeof(location));
    assert(h2_pal_modem_cell_locate(platform, 0u, &location) == H2_PAL_OK);
    assert(location.valid == 1u);
    assert(location.latitude_e7 == 318476490);
    assert(location.longitude_e7 == 1172001340);
    assert(location.accuracy_m == 120u);

    assert(state.count == 2u);
    assert(strcmp(state.records[0].cmd, "AT+QLBSCFG=\"token\",\"" FAKE_TOKEN "\"") == 0);
    assert(strcmp(state.records[1].cmd, "AT+QLBS") == 0);
    assert(state.records[0].timeout_ms == 45000u);
    assert(state.records[1].timeout_ms == 45000u);

    /* A second query reuses the token already configured in the modem. */
    memset(&location, 0, sizeof(location));
    assert(h2_pal_modem_cell_locate(platform, 7000u, &location) == H2_PAL_OK);
    assert(location.valid == 1u);
    assert(state.count == 3u);
    assert(strcmp(state.records[2].cmd, "AT+QLBS") == 0);
    assert(state.records[2].timeout_ms == 7000u);
    assert(count_commands_with(&state, "QLBSCFG") == 1u);

    /* The token exists only in the one command that has to carry it, and
     * never in modem state. */
    assert(count_commands_with(&state, FAKE_TOKEN) == 1u);
    assert(find_command(&state, FAKE_TOKEN) == &state.records[0]);
    assert(!bytes_contain(&modem, sizeof(modem), FAKE_TOKEN));
    assert(!bytes_contain(&location, sizeof(location), FAKE_TOKEN));

    /* close() drops the modem-side configuration, so the next query has to
     * send the token again. */
    assert(h2_pal_modem_open(platform, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_close(platform, 1000u) == H2_PAL_OK);
    state.count = 0u;
    assert(h2_pal_modem_cell_locate(platform, 0u, &location) == H2_PAL_OK);
    assert(state.count == 2u);
    assert(strcmp(state.records[0].cmd, "AT+QLBSCFG=\"token\",\"" FAKE_TOKEN "\"") == 0);
    assert(strcmp(state.records[1].cmd, "AT+QLBS") == 0);
    assert(state.records[0].timeout_ms == 45000u);

    h2_quectel_modem_deinit(&modem);
}

static void test_locate_without_token(void) {
    transport_state_t state;
    memset(&state, 0, sizeof(state));

    h2_quectel_modem_config_t config;
    init_config(&config, &state);

    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    h2_pal_modem_t *platform = h2_quectel_modem_platform(&modem);

    uint32_t capabilities = 0u;
    assert(h2_pal_modem_get_capabilities(platform, &capabilities) == H2_PAL_OK);
    assert((capabilities & H2_PAL_MODEM_CAPABILITY_CELL_LOCATE) == 0u);
    assert(platform->vtable->cell_locate == NULL);

    h2_pal_modem_cell_location_t location;
    memset(&location, 0xAA, sizeof(location));
    assert(h2_pal_modem_cell_locate(platform, 0u, &location) == H2_PAL_ERR_UNSUPPORTED);
    assert(location.valid == 0u);
    assert(location.latitude_e7 == 0);
    assert(location.longitude_e7 == 0);
    assert(location.accuracy_m == 0u);
    assert(state.count == 0u);
    h2_quectel_modem_deinit(&modem);

    /* An empty token is the same as no token. */
    memset(&state, 0, sizeof(state));
    init_config(&config, &state);
    config.cell_locate_token = "";
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    platform = h2_quectel_modem_platform(&modem);
    assert(h2_pal_modem_get_capabilities(platform, &capabilities) == H2_PAL_OK);
    assert((capabilities & H2_PAL_MODEM_CAPABILITY_CELL_LOCATE) == 0u);
    assert(h2_pal_modem_cell_locate(platform, 0u, &location) == H2_PAL_ERR_UNSUPPORTED);
    assert(count_commands_with(&state, "QLBS") == 0u);
    h2_quectel_modem_deinit(&modem);

    /* An explicitly requested capability bit cannot conjure the token. */
    memset(&state, 0, sizeof(state));
    init_config(&config, &state);
    config.capabilities = H2_PAL_MODEM_CAPABILITY_CALL | H2_PAL_MODEM_CAPABILITY_CELL_LOCATE;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    platform = h2_quectel_modem_platform(&modem);
    assert(h2_pal_modem_get_capabilities(platform, &capabilities) == H2_PAL_OK);
    assert((capabilities & H2_PAL_MODEM_CAPABILITY_CELL_LOCATE) == 0u);
    assert(h2_pal_modem_cell_locate(platform, 0u, &location) == H2_PAL_ERR_UNSUPPORTED);
    assert(count_commands_with(&state, "QLBS") == 0u);
    h2_quectel_modem_deinit(&modem);
}

static void test_token_that_breaks_command_framing(void) {
    transport_state_t state;
    memset(&state, 0, sizeof(state));

    h2_quectel_modem_config_t config;
    init_config(&config, &state);
    h2_quectel_modem_t modem;

    static const char *const rejected[] = {
        "FAKE\"TOKEN",
        "FAKE,TOKEN",
        "FAKE\rTOKEN",
        "FAKE\nTOKEN",
    };
    for (size_t i = 0u; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        config.cell_locate_token = rejected[i];
        assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_ERR_INVALID_ARG);
    }

    char too_long[H2_QUECTEL_CELL_LOCATE_TOKEN_MAX + 2u];
    memset(too_long, 'F', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';
    config.cell_locate_token = too_long;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_ERR_INVALID_ARG);
    assert(state.count == 0u);
}

static void check_failure(
    const char *qlbs_response,
    h2_pal_result_t qlbs_result,
    h2_pal_result_t expected) {
    transport_state_t state;
    memset(&state, 0, sizeof(state));
    state.qlbs_response = qlbs_response;
    state.qlbs_result = qlbs_result;

    h2_quectel_modem_config_t config;
    init_config(&config, &state);
    config.cell_locate_token = FAKE_TOKEN;

    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    h2_pal_modem_t *platform = h2_quectel_modem_platform(&modem);

    h2_pal_modem_cell_location_t location;
    memset(&location, 0xAA, sizeof(location));
    assert(h2_pal_modem_cell_locate(platform, 0u, &location) == expected);
    assert(location.valid == 0u);
    assert(location.latitude_e7 == 0);
    assert(location.longitude_e7 == 0);
    assert(location.accuracy_m == 0u);
    h2_quectel_modem_deinit(&modem);
}

static void test_service_and_format_errors(void) {
    /* Positioning failed on the server side. */
    check_failure("+QLBS: 10000\r\nERROR\r\n", H2_PAL_ERR_IO, H2_PAL_ERR_UNAVAILABLE);
    /* The token does not exist, or has expired. */
    check_failure("+QLBS: 10002\r\nERROR\r\n", H2_PAL_ERR_IO, H2_PAL_ERR_INVALID_ARG);
    check_failure("+QLBS: 10006\r\nERROR\r\n", H2_PAL_ERR_IO, H2_PAL_ERR_INVALID_ARG);
    /* The service refused the device identity. */
    check_failure("+QLBS: 10001\r\nERROR\r\n", H2_PAL_ERR_IO, H2_PAL_ERR_INVALID_STATE);
    /* Quota and rate limits. */
    check_failure("+QLBS: 10009\r\nERROR\r\n", H2_PAL_ERR_IO, H2_PAL_ERR_BUSY);
    /* The server did not answer in time. */
    check_failure("+QLBS: 702\r\nERROR\r\n", H2_PAL_ERR_IO, H2_PAL_ERR_TIMEOUT);
    /* Packet data is not usable, reported by the module as an ME error. */
    check_failure(
        "+CME ERROR: operation not allowed\r\n",
        H2_PAL_ERR_IO,
        H2_PAL_ERR_INVALID_STATE);
    check_failure("+CME ERROR: 3\r\n", H2_PAL_ERR_IO, H2_PAL_ERR_INVALID_STATE);
    /* Malformed responses. */
    check_failure("+QLBS: 0,117.200134\r\nOK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
    check_failure("+QLBS: 0,north,east\r\nOK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
    check_failure("+QLBS: 0,nan,31.847649\r\nOK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
    check_failure("+QLBS: 0,117.200134,inf\r\nOK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
    check_failure("+QLBS: 0,131.847649,117.200134\r\nOK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
    /* Latitude-first output (the old assumption) is rejected, not misread. */
    check_failure("+QLBS: 0,31.847649,117.200134\r\nOK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
    check_failure("+QLBS: 0,200.000000,31.847649\r\nOK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
    check_failure("+QLBS: ,117.200134,31.847649\r\nOK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
    check_failure("OK\r\n", H2_PAL_OK, H2_PAL_ERR_FORMAT);
}

static void test_optional_field_and_negative_degrees(void) {
    transport_state_t state;
    memset(&state, 0, sizeof(state));
    /* Modules without an accuracy field report a quoted server timestamp
     * instead; it must not be read as accuracy. */
    state.qlbs_response = "+QLBS: 0,-117.200134,-31.847649,\"21/09/26\r\nOK\r\n";
    state.qlbs_result = H2_PAL_OK;

    h2_quectel_modem_config_t config;
    init_config(&config, &state);
    config.cell_locate_token = FAKE_TOKEN;

    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    h2_pal_modem_t *platform = h2_quectel_modem_platform(&modem);

    h2_pal_modem_cell_location_t location;
    assert(h2_pal_modem_cell_locate(platform, 0u, &location) == H2_PAL_OK);
    assert(location.valid == 1u);
    assert(location.latitude_e7 == -318476490);
    assert(location.longitude_e7 == -1172001340);
    assert(location.accuracy_m == 0u);
    /* An unset cell_locate_timeout_ms falls back to the provider default. */
    assert(state.records[1].timeout_ms == H2_QUECTEL_CELL_LOCATE_TIMEOUT_MS);
    h2_quectel_modem_deinit(&modem);
}

static void test_token_never_leaks_back(void) {
    transport_state_t state;
    memset(&state, 0, sizeof(state));
    /* A module that echoes the token back must not leave it anywhere the
     * caller can read. */
    state.qlbscfg_response = "+QLBSCFG: \"token\",\"" FAKE_TOKEN "\"\r\nOK\r\n";
    state.qlbscfg_result = H2_PAL_OK;
    state.qlbs_response = "+QLBS: 0,117.200134,31.847649\r\nOK\r\n";
    state.qlbs_result = H2_PAL_OK;

    h2_quectel_modem_config_t config;
    init_config(&config, &state);
    config.cell_locate_token = FAKE_TOKEN;

    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    h2_pal_modem_t *platform = h2_quectel_modem_platform(&modem);

    h2_pal_modem_cell_location_t location;
    memset(&location, 0xAA, sizeof(location));
    assert(h2_pal_modem_cell_locate(platform, 0u, &location) == H2_PAL_ERR_IO);
    assert(location.valid == 0u);
    assert(!bytes_contain(&modem, sizeof(modem), FAKE_TOKEN));
    assert(!bytes_contain(&location, sizeof(location), FAKE_TOKEN));
    assert(count_commands_with(&state, "QLBS") == 1u);
    h2_quectel_modem_deinit(&modem);
}

static void test_gnss_path_unchanged(void) {
    transport_state_t state;
    memset(&state, 0, sizeof(state));

    h2_quectel_modem_config_t config;
    init_config(&config, &state);
    config.cell_locate_token = FAKE_TOKEN;

    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    h2_pal_modem_t *platform = h2_quectel_modem_platform(&modem);

    assert(h2_pal_modem_gnss_start(platform, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_gnss_stop(platform, 1000u) == H2_PAL_OK);
    assert(state.count == 2u);
    assert(strcmp(state.records[0].cmd, "AT+QGPS=1") == 0);
    assert(strcmp(state.records[1].cmd, "AT+QGPSEND") == 0);
    assert(count_commands_with(&state, "QLBS") == 0u);
    h2_quectel_modem_deinit(&modem);
}

static void test_locate_rejects_known_absent_sim(void) {
    transport_state_t state = {0};
    h2_quectel_modem_config_t config;
    init_config(&config, &state);
    config.cell_locate_token = FAKE_TOKEN;
    h2_quectel_modem_t modem;
    assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
    h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,0");
    h2_pal_modem_cell_location_t location;
    assert(h2_pal_modem_cell_locate(&modem.platform, 0u, &location) == H2_PAL_ERR_INVALID_STATE);
    assert(!location.valid && state.count == 0u);
    h2_quectel_modem_deinit(&modem);
}

int main(void) {
    test_locate_rejects_known_absent_sim();
    test_locate_sends_token_once();
    test_locate_without_token();
    test_token_that_breaks_command_framing();
    test_service_and_format_errors();
    test_optional_field_and_negative_degrees();
    test_token_never_leaks_back();
    test_gnss_path_unchanged();
    return 0;
}
