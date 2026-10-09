#include "h2_quectel_modem.h"
#include "h2_simcom_modem.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_mutex { unsigned depth; };
static h2_pal_result_t create_mutex(void *user, const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out) {
    (void)user;
    assert(config->flags == H2_PAL_MUTEX_FLAG_RECURSIVE);
    *out = calloc(1u, sizeof(**out));
    assert(*out != NULL);
    return H2_PAL_OK;
}
static h2_pal_result_t destroy_mutex(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(mutex->depth == 0u);
    free(mutex);
    return H2_PAL_OK;
}
static h2_pal_result_t lock_mutex(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    mutex->depth++;
    return H2_PAL_OK;
}
static h2_pal_result_t unlock_mutex(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(mutex->depth > 0u);
    mutex->depth--;
    return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = create_mutex, .destroy_mutex = destroy_mutex,
    .lock_mutex = lock_mutex, .unlock_mutex = unlock_mutex,
};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};

typedef struct fixture {
    h2_quectel_modem_t modem;
    const char *reply;
    const char *raw_reply;
    size_t raw_offset;
    h2_pal_result_t query_error;
    h2_pal_result_t wake_error;
    unsigned queries, wakes;
    uint32_t query_timeout_ms;
    int asleep, invalidate, unterminated;
} fixture_t;

static void assert_transport_context(fixture_t *f) {
    assert(f->modem.operation_lock->depth > 0u);
    assert(f->modem.lock->depth == 0u);
    assert(!f->asleep);
}
static h2_pal_result_t gate(void *user, int allow_sleep) {
    fixture_t *f = user;
    if (!allow_sleep) {
        f->wakes++;
        if (f->wake_error != H2_PAL_OK) { return f->wake_error; }
    }
    f->asleep = allow_sleep;
    return H2_PAL_OK;
}
static const char *reply_for(fixture_t *f, const char *cmd) {
    if (strcmp(cmd, "AT+QECCNUM?") == 0) {
        f->queries++;
        return f->reply;
    }
    if (strcmp(cmd, "AT+CGMM") == 0) { return "EC25\r\nOK\r\n"; }
    return "OK\r\n";
}
static h2_pal_result_t command(void *user, const char *cmd, char *response,
    size_t size, uint32_t timeout_ms) {
    fixture_t *f = user;
    assert_transport_context(f);
    const char *reply = reply_for(f, cmd);
    if (strcmp(cmd, "AT+QECCNUM?") == 0) {
        f->query_timeout_ms = timeout_ms;
        if (f->invalidate == 1) { h2_quectel_handle_urc_line(&f->modem, "RDY"); }
        if (f->invalidate == 2) { h2_quectel_handle_urc_line(&f->modem, "+QSIMSTAT: 1,0"); }
        if (f->unterminated) {
            memset(response, 'x', size);
            return H2_PAL_OK;
        }
    }
    assert(strlen(reply) < size);
    memcpy(response, reply, strlen(reply) + 1u);
    return strcmp(cmd, "AT+QECCNUM?") == 0 ? f->query_error : H2_PAL_OK;
}
static h2_pal_result_t write_bytes(void *user, const uint8_t *data, size_t size,
    uint32_t timeout_ms, size_t *out_size) {
    fixture_t *f = user;
    assert_transport_context(f);
    assert(size > 0u && size < H2_QUECTEL_LINE_MAX && data[size - 1u] == '\r');
    char cmd[H2_QUECTEL_LINE_MAX];
    memcpy(cmd, data, size - 1u);
    cmd[size - 1u] = '\0';
    f->raw_reply = reply_for(f, cmd);
    f->raw_offset = 0u;
    f->query_timeout_ms = timeout_ms;
    *out_size = size;
    return H2_PAL_OK;
}
static h2_pal_result_t read_bytes(void *user, uint8_t *data, size_t size,
    uint32_t timeout_ms, size_t *out_size) {
    fixture_t *f = user;
    (void)timeout_ms;
    assert_transport_context(f);
    assert(size == 1u);
    if (f->raw_reply[f->raw_offset] == '\0') {
        *out_size = 0u;
        return H2_PAL_ERR_TIMEOUT;
    }
    data[0] = (uint8_t)f->raw_reply[f->raw_offset++];
    *out_size = 1u;
    return H2_PAL_OK;
}
static void assert_released(fixture_t *f) {
    assert(f->modem.operation_depth == 0u);
    assert(f->modem.operation_lock->depth == 0u);
    assert(f->modem.lock->depth == 0u);
}
static void init(fixture_t *f, int low_power, int raw) {
    memset(f, 0, sizeof(*f));
    /* Deliberately differs from the manual: actual reply includes 120. */
    f->reply = "+QECCNUM: 0,\"110\",\"120\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n";
    const h2_quectel_modem_config_t config = {
        .command = raw ? NULL : command, .transport_user = f, .sync_api = &sync_api,
        .read = raw ? read_bytes : NULL, .write = raw ? write_bytes : NULL,
        .profile = low_power ? H2_QUECTEL_MODEM_PROFILE_EC25_UART : 0,
        .sleep_gate = low_power ? gate : NULL,
    };
    assert(h2_quectel_modem_init(&f->modem, &config) == H2_PAL_OK);
    uint32_t capabilities = 0u;
    assert(h2_pal_modem_get_capabilities(&f->modem.platform, &capabilities) == H2_PAL_OK);
    assert(capabilities & H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS);
    assert(h2_pal_modem_open(&f->modem.platform, 1000u) == H2_PAL_OK);
    assert(f->queries == 0u); /* No discovery or configuration during open. */
    assert_released(f);
}
static void query(fixture_t *f, h2_pal_result_t expected, size_t capacity,
    size_t expected_count) {
    h2_pal_modem_emergency_number_t numbers[40];
    assert(capacity <= 40u);
    memset(numbers, 0xff, sizeof(numbers));
    size_t count = 99u;
    const uint8_t call_hold = f->modem.call_hold;
    assert(h2_pal_modem_get_emergency_numbers(&f->modem.platform, 731u,
        numbers, capacity, &count) == expected);
    assert(count == (expected == H2_PAL_OK ? expected_count : 0u));
    if (expected == H2_PAL_OK) {
        for (size_t i = 0u; i < count; i++) {
            assert(numbers[i].source == H2_PAL_MODEM_EMERGENCY_SOURCE_MODULE);
            assert(numbers[i].categories_valid == 0u && numbers[i].categories == 0u);
        }
    } else {
        for (size_t i = 0u; i < capacity; i++) {
            assert(numbers[i].number[0] == '\0');
            assert(numbers[i].scope == H2_PAL_MODEM_EMERGENCY_SCOPE_UNKNOWN);
        }
    }
    assert(f->modem.call_hold == call_hold);
    assert(f->modem.config.command_timeout_ms == 5000u);
    assert(f->modem.config.io_timeout_ms == 200u);
    assert_released(f);
}
static void finish(fixture_t *f) {
    assert(h2_quectel_modem_deinit(&f->modem) == H2_PAL_OK);
}

static void test_unsupported(void) {
    h2_pal_modem_emergency_number_t numbers[1];
    size_t count = 99u;
    assert(h2_pal_modem_get_emergency_numbers(h2_pal_unsupported_modem_api(),
        0u, numbers, 1u, &count) == H2_PAL_ERR_UNSUPPORTED);
    h2_simcom_modem_t modem;
    const h2_simcom_modem_config_t config = {
        .command = command, .capabilities = H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS,
    };
    assert(h2_simcom_modem_init(&modem, &config) == H2_PAL_OK);
    uint32_t capabilities = 99u;
    assert(h2_pal_modem_get_capabilities(&modem.platform, &capabilities) == H2_PAL_OK);
    assert((capabilities & H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS) == 0u);
    assert(h2_pal_modem_get_emergency_numbers(&modem.platform, 0u,
        numbers, 1u, &count) == H2_PAL_ERR_UNSUPPORTED);
    assert(count == 0u);
    assert(h2_simcom_modem_deinit(&modem) == H2_PAL_OK);
}

static void test_tables_and_lifecycle(void) {
    fixture_t f;
    init(&f, 0, 0);
    f.modem.sim_state = H2_PAL_MODEM_SIM_STATE_ABSENT;
    f.modem.observed_status.registration = H2_PAL_MODEM_REGISTRATION_OFFLINE;
    h2_pal_modem_emergency_number_t numbers[40];
    size_t count;
    assert(h2_pal_modem_get_emergency_numbers(&f.modem.platform, 731u,
        numbers, 40u, &count) == H2_PAL_OK);
    assert(count == 3u && strcmp(numbers[1].number, "120") == 0);
    assert(numbers[0].scope == H2_PAL_MODEM_EMERGENCY_SCOPE_WITHOUT_SIM);
    assert(numbers[2].scope == H2_PAL_MODEM_EMERGENCY_SCOPE_WITH_SIM);
    assert(f.query_timeout_ms == 731u);
    f.reply = "+QECCNUM: 1, \"999\"\r\n+QECCNUM: 0, \"000\" , \"08\"\r\nOK\r\n";
    query(&f, H2_PAL_OK, 40u, 3u);
    assert(h2_pal_modem_get_emergency_numbers(&f.modem.platform, 0u,
        numbers, 40u, &count) == H2_PAL_OK);
    assert(strcmp(numbers[0].number, "999") == 0 && strcmp(numbers[1].number, "000") == 0);
    assert(f.query_timeout_ms == 5000u); /* No cached table or timeout. */
    query(&f, H2_PAL_ERR_TRUNCATED, 2u, 0u);
    f.reply = "+QECCNUM: 0\r\n+QECCNUM: 1\r\nOK\r\n";
    query(&f, H2_PAL_OK, 40u, 0u);
    unsigned calls = f.queries;
    assert(h2_pal_modem_close(&f.modem.platform, 1000u) == H2_PAL_OK);
    query(&f, H2_PAL_ERR_CLOSED, 40u, 0u);
    assert(f.queries == calls);
    assert(h2_pal_modem_open(&f.modem.platform, 1000u) == H2_PAL_OK);
    f.reply = "+QECCNUM: 0,\"112\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n";
    query(&f, H2_PAL_OK, 40u, 2u);
    finish(&f);
}

static void test_failure_and_generation(void) {
    fixture_t f;
    init(&f, 0, 0);
    f.reply = "OK\r\n";
    query(&f, H2_PAL_ERR_TRUNCATED, 40u, 0u);
    f.reply = "+QECCNUM: 0,\"110\"\r\nOK\r\n";
    query(&f, H2_PAL_ERR_TRUNCATED, 40u, 0u);
    f.reply = "+QECCNUM: 0,\"110\"\r\n+QECCNUM: 1,\"112\"\r\n";
    query(&f, H2_PAL_ERR_TRUNCATED, 40u, 0u);
    const char *malformed[] = {
        "+QECCNUM: 0,\"110\"\r\n+QECCNUM: 0,\"112\"\r\nOK\r\n",
        "+QECCNUM: 2,\"110\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n",
        "+QECCNUM: 00,\"110\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n",
        "+QECCNUM: 0,\"\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n",
        "+QECCNUM: 0,110\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n",
        "+QECCNUM: 0,\"11x\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n",
        "+QECCNUM: 0,\"+110\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n",
        "+QECCNUM: 0,\"110\",\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n",
        "+QECCNUM: 0,\"110\"junk\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n",
        "+QECCNUM: 0,\"110\"\r\n+QECCNUM: 1,\"112\"\r\nunexpected\r\nOK\r\n",
    };
    for (size_t i = 0u; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        f.reply = malformed[i];
        query(&f, H2_PAL_ERR_FORMAT, 40u, 0u);
    }
    f.reply = "+QECCNUM: 0,\"12345678901234567890123456789012\"\r\n+QECCNUM: 1\r\nOK\r\n";
    query(&f, H2_PAL_ERR_TRUNCATED, 40u, 0u);
    f.reply = "ERROR\r\n";
    query(&f, H2_PAL_ERR_IO, 40u, 0u);
    f.reply = "+QECCNUM: 0,\"110\"\r\n+QECCNUM: 1,\"112\"\r\n+CME ERROR: 4\r\n";
    query(&f, H2_PAL_ERR_IO, 40u, 0u);
    f.reply = "+QECCNUM: 0,\"110\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n";
    const h2_pal_result_t errors[] = {H2_PAL_ERR_TIMEOUT, H2_PAL_ERR_IO, H2_PAL_ERR_UNSUPPORTED};
    for (size_t i = 0u; i < sizeof(errors) / sizeof(errors[0]); i++) {
        f.query_error = errors[i];
        query(&f, errors[i], 40u, 0u);
    }
    f.query_error = H2_PAL_OK;
    f.unterminated = 1;
    query(&f, H2_PAL_ERR_TRUNCATED, 40u, 0u);
    f.unterminated = 0;
    f.invalidate = 1;
    query(&f, H2_PAL_ERR_INVALID_STATE, 40u, 0u);
    f.invalidate = 2;
    query(&f, H2_PAL_ERR_INVALID_STATE, 40u, 0u);
    f.invalidate = 0;
    query(&f, H2_PAL_OK, 40u, 2u);
    f.reply = "+QECCNUM: 0,\"110\"\r\nRDY\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n";
    query(&f, H2_PAL_ERR_INVALID_STATE, 40u, 0u);
    f.reply = "+QECCNUM: 0,\"110\"\r\n+QSIMSTAT: 1,0\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n";
    query(&f, H2_PAL_ERR_INVALID_STATE, 40u, 0u);
    finish(&f);
}

static void test_long_and_raw_replies(void) {
    char reply[H2_QUECTEL_AT_LINE_MAX * H2_QUECTEL_RESPONSE_MAX];
    size_t length = 0u;
    for (unsigned type = 0u; type < 2u; type++) {
        length += (size_t)snprintf(reply + length, sizeof(reply) - length, "+QECCNUM: %u", type);
        for (unsigned i = 0u; i < 20u; i++) {
            length += (size_t)snprintf(reply + length, sizeof(reply) - length,
                ",\"1234567890123456789012345678901\"");
        }
        length += (size_t)snprintf(reply + length, sizeof(reply) - length, "\r\n");
    }
    (void)snprintf(reply + length, sizeof(reply) - length, "OK\r\n");
    for (int raw = 0; raw < 2; raw++) {
        fixture_t f;
        init(&f, 0, raw);
        f.reply = reply;
        query(&f, H2_PAL_OK, 40u, 40u);
        query(&f, H2_PAL_ERR_TRUNCATED, 39u, 0u);
        f.reply = "+QECCNUM: 0,\"bad\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n";
        query(&f, H2_PAL_ERR_FORMAT, 40u, 0u);
        if (raw) { assert(f.raw_reply[f.raw_offset] == '\0'); }
        f.reply = "+QECCNUM: 0\r\n+QECCNUM: 1,\"999\"\r\nOK\r\n";
        query(&f, H2_PAL_OK, 40u, 1u);
        finish(&f);
    }
    /* An overlong transport line must not be accepted as an empty table. */
    memset(reply, '1', H2_QUECTEL_AT_LINE_MAX + 10u);
    memcpy(reply, "+QECCNUM: 0,\"", 13u);
    strcpy(reply + H2_QUECTEL_AT_LINE_MAX + 10u, "\"\r\n+QECCNUM: 1\r\nOK\r\n");
    fixture_t f;
    init(&f, 0, 0);
    f.reply = reply;
    query(&f, H2_PAL_ERR_TRUNCATED, 40u, 0u);
    finish(&f);
}

static void test_sleep(void) {
    fixture_t f;
    init(&f, 1, 0);
    assert(h2_pal_modem_set_power_policy(&f.modem.platform,
        H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) == H2_PAL_OK);
    assert(f.asleep);
    unsigned wakes = f.wakes;
    query(&f, H2_PAL_OK, 40u, 3u);
    assert(f.wakes > wakes && f.asleep);
    f.wake_error = H2_PAL_ERR_IO;
    unsigned calls = f.queries;
    query(&f, H2_PAL_ERR_IO, 40u, 0u);
    assert(f.queries == calls);
    f.wake_error = H2_PAL_OK;
    finish(&f);
}

int main(void) {
    test_unsupported();
    test_tables_and_lifecycle();
    test_failure_and_generation();
    test_long_and_raw_replies();
    test_sleep();
    return 0;
}
