#include "h2_quectel_modem.h"

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

typedef struct phonebook_entry { unsigned index; const char *number; } phonebook_entry_t;
typedef struct fixture {
    h2_quectel_modem_t modem;
    const char *model, *supported, *ranges, *read_reply, *quality_reply;
    const char *fail_command, *fail_reply, *invalidate_command;
    const char *missing_ok_command;
    unsigned fail_at, fail_seen;
    h2_pal_result_t failure;
    int apply_failed_selection, invalidate;
    int asleep;
    char storage[16];
    phonebook_entry_t entries[64];
    size_t entry_count;
    unsigned used, total;
    char commands[128][96];
    uint32_t timeouts[128];
    unsigned count, models;
} fixture_t;

static h2_pal_result_t command(void *user, const char *cmd, char *response,
    size_t size, uint32_t timeout_ms) {
    fixture_t *f = user;
    assert(f->modem.operation_lock->depth > 0u && f->modem.lock->depth == 0u);
    assert(!f->asleep);
    assert(f->count < 128u && strlen(cmd) < sizeof(f->commands[0]));
    strcpy(f->commands[f->count], cmd);
    f->timeouts[f->count++] = timeout_ms;
    int failing = f->fail_command != NULL && strcmp(cmd, f->fail_command) == 0 &&
        ++f->fail_seen == f->fail_at;
    response[0] = '\0';
    if (strcmp(cmd, "AT+CGMM") == 0) {
        f->models++;
        (void)snprintf(response, size, "%s\r\nOK\r\n", f->model);
    } else if (strcmp(cmd, "AT+CPBS=?") == 0) {
        (void)snprintf(response, size, "%s\r\nOK\r\n", f->supported);
    } else if (strcmp(cmd, "AT+CPBS?") == 0) {
        (void)snprintf(response, size, "+CPBS: \"%s\",%u,%u\r\nOK\r\n", f->storage,
            strcmp(f->storage, "EN") == 0 ? f->used : 10u,
            strcmp(f->storage, "EN") == 0 ? f->total : 100u);
    } else if (strncmp(cmd, "AT+CPBS=\"", 9u) == 0) {
        char storage[16];
        assert(sscanf(cmd, "AT+CPBS=\"%15[^\"]\"", storage) == 1);
        if (!failing || f->apply_failed_selection) { strcpy(f->storage, storage); }
        strcpy(response, "OK\r\n");
    } else if (strcmp(cmd, "AT+CPBR=?") == 0) {
        (void)snprintf(response, size, "%s\r\nOK\r\n", f->ranges);
    } else if (strncmp(cmd, "AT+CPBR=", 8u) == 0) {
        unsigned first, last;
        assert(sscanf(cmd, "AT+CPBR=%u,%u", &first, &last) == 2);
        if (f->read_reply != NULL) {
            assert(strlen(f->read_reply) < size);
            strcpy(response, f->read_reply);
        } else {
            size_t length = 0u;
            for (size_t i = 0u; i < f->entry_count; i++) {
                if (f->entries[i].index < first || f->entries[i].index > last) { continue; }
                int added = snprintf(response + length, size - length,
                    "+CPBR: %u,\"%s\",129,\"label, name\"\r\n", f->entries[i].index, f->entries[i].number);
                assert(added > 0 && (size_t)added < size - length);
                length += (size_t)added;
            }
            strcpy(response + length, "OK\r\n");
        }
    } else if (strcmp(cmd, "AT+CSQ") == 0) {
        strcpy(response, "+CSQ: 20,99\r\nOK\r\n");
    } else if (strcmp(cmd, "AT+QENG=\"servingcell\"") == 0) {
        assert(f->quality_reply != NULL && strlen(f->quality_reply) < size);
        strcpy(response, f->quality_reply);
    } else if (strcmp(cmd, "AT+QGPSLOC=0") == 0) {
        strcpy(response, "+QGPSLOC: 120000.00,3000.0000N,12000.0000E,1.2,12.3,3,90.0,36.0,19.438,081026,10\r\nOK\r\n");
    } else if (strcmp(cmd, "AT+QECCNUM?") == 0) {
        assert(strncmp(f->model, "EC25", 4u) == 0);
        strcpy(response, "+QECCNUM: 0,\"110\"\r\n+QECCNUM: 1,\"112\"\r\nOK\r\n");
    } else {
        assert(strcmp(cmd, "AT+QCSQ") != 0);
        assert(strncmp(cmd, "AT+QLBS", 7u) != 0);
        strcpy(response, "OK\r\n");
    }
    if (f->invalidate_command != NULL && strcmp(cmd, f->invalidate_command) == 0) {
        h2_quectel_handle_urc_line(&f->modem, f->invalidate == 1 ? "RDY" : "+QSIMSTAT: 1,0");
        f->invalidate_command = NULL;
    }
    if (failing) {
        if (f->fail_reply != NULL) { strcpy(response, f->fail_reply); }
        return f->failure;
    }
    if (f->missing_ok_command != NULL && strcmp(cmd, f->missing_ok_command) == 0) {
        char *ok = strstr(response, "OK\r\n");
        assert(ok != NULL);
        *ok = '\0';
    }
    return H2_PAL_OK;
}

static unsigned calls(const fixture_t *f, const char *command_prefix) {
    unsigned count = 0u;
    for (unsigned i = 0u; i < f->count; i++) {
        if (strncmp(f->commands[i], command_prefix, strlen(command_prefix)) == 0) { count++; }
    }
    return count;
}
static void assert_released(const fixture_t *f) {
    assert(f->modem.operation_depth == 0u);
    assert(f->modem.operation_lock->depth == 0u && f->modem.lock->depth == 0u);
}
static h2_pal_result_t gate(void *user, int allow_sleep) {
    ((fixture_t *)user)->asleep = allow_sleep;
    return H2_PAL_OK;
}

static void init_with_power(fixture_t *f, const char *model,
    h2_quectel_modem_profile_t profile, int low_power) {
    memset(f, 0, sizeof(*f));
    f->model = model;
    f->supported = "+CPBS: (\"SM\",\"ME\",\"EN\")";
    f->ranges = "+CPBR: (1-3),20,14";
    strcpy(f->storage, "SM");
    f->entries[0] = (phonebook_entry_t){1u, "110"};
    f->entries[1] = (phonebook_entry_t){3u, "120"};
    f->entry_count = f->used = 2u;
    f->total = 3u;
    f->fail_at = 1u;
    const h2_quectel_modem_config_t config = {
        .command = command, .transport_user = f, .sync_api = &sync_api,
        .profile = profile, .cell_locate_token = "FAKE-TEST-TOKEN",
        .sleep_gate = low_power ? gate : NULL,
    };
    assert(h2_quectel_modem_init(&f->modem, &config) == H2_PAL_OK);
    assert(h2_pal_modem_open(&f->modem.platform, 0u) == H2_PAL_OK);
    f->count = 0u;
    assert_released(f);
}
static void init(fixture_t *f, const char *model, h2_quectel_modem_profile_t profile) {
    init_with_power(f, model, profile, 0);
}
static void query(fixture_t *f, h2_pal_result_t expected, size_t capacity, size_t expected_count) {
    h2_pal_modem_emergency_number_t numbers[64];
    size_t count = 99u;
    memset(numbers, 0xff, sizeof(numbers));
    assert(capacity <= 64u);
    h2_pal_result_t result = h2_pal_modem_get_emergency_numbers(&f->modem.platform, 777u,
        numbers, capacity, &count);
    if (result != expected) {
        fprintf(stderr, "model=%s expected=%d actual=%d storage=%s pending=%u\n",
            f->model, expected, result, f->storage, f->modem.phonebook_restore_pending);
        for (unsigned i = f->count > 12u ? f->count - 12u : 0u; i < f->count; i++) {
            fprintf(stderr, "%s\n", f->commands[i]);
        }
    }
    assert(result == expected);
    assert(count == expected_count);
    if (expected == H2_PAL_OK && f->modem.family == H2_QUECTEL_MODEM_FAMILY_EC800M) {
        for (size_t i = 0u; i < count; i++) {
            assert(strcmp(numbers[i].number, f->entries[i].number) == 0);
            assert(numbers[i].scope == H2_PAL_MODEM_EMERGENCY_SCOPE_UNKNOWN);
            assert(numbers[i].source == H2_PAL_MODEM_EMERGENCY_SOURCE_UNKNOWN);
            assert(!numbers[i].categories_valid);
        }
        assert(strcmp(f->storage, "SM") == 0 || strcmp(f->storage, "EN") == 0);
        assert(!f->modem.phonebook_restore_pending);
    } else if (expected != H2_PAL_OK) {
        assert(count == 0u);
        for (size_t i = 0u; i < capacity; i++) { assert(numbers[i].number[0] == '\0'); }
    }
    assert(f->modem.config.command_timeout_ms == 5000u && f->modem.config.io_timeout_ms == 200u);
    assert_released(f);
}
static void test_missing_terminal_ok(void) {
    const char *commands[] = {
        "AT+CPBS=?", "AT+CPBS?", "AT+CPBS=\"EN\"", "AT+CPBR=?", "AT+CPBR=1,3", "AT+CPBS=\"SM\"",
    };
    for (size_t i = 0u; i < sizeof(commands) / sizeof(commands[0]); i++) {
        fixture_t f;
        init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_EC800M_UART);
        f.missing_ok_command = commands[i];
        query(&f, H2_PAL_ERR_TRUNCATED, 64u, 0u);
        if (strcmp(commands[i], "AT+CPBS=\"SM\"") == 0) {
            assert(f.modem.phonebook_restore_pending);
        }
        f.missing_ok_command = NULL;
        query(&f, H2_PAL_OK, 64u, 2u);
        assert(!f.modem.phonebook_restore_pending && strcmp(f.storage, "SM") == 0);
        assert(h2_quectel_modem_deinit(&f.modem) == H2_PAL_OK);
    }
}
static void finish(fixture_t *f) {
    f->fail_command = NULL;
    assert(h2_quectel_modem_deinit(&f->modem) == H2_PAL_OK);
}

static void test_models(void) {
    const h2_quectel_modem_profile_t profiles[] = {
        H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED,
        H2_QUECTEL_MODEM_PROFILE_EC25_UART, H2_QUECTEL_MODEM_PROFILE_EC800M_UART,
    };
    for (size_t i = 0u; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        fixture_t f;
        init(&f, "EC800M-CN", profiles[i]);
        f.modem.sim_seen = 1u;
        f.modem.sim_state = H2_PAL_MODEM_SIM_STATE_ABSENT;
        query(&f, H2_PAL_OK, 64u, 2u);
        assert(calls(&f, "AT+QECCNUM") == 0u && f.models == 1u);
        assert(f.modem.family == H2_QUECTEL_MODEM_FAMILY_EC800M);
        uint32_t capabilities;
        assert(h2_pal_modem_get_capabilities(&f.modem.platform, &capabilities) == H2_PAL_OK);
        assert(!(capabilities & H2_PAL_MODEM_CAPABILITY_CELL_LOCATE));
        for (unsigned j = 0u; j < f.count; j++) {
            if (strncmp(f.commands[j], "AT+CPB", 6u) == 0) { assert(f.timeouts[j] == 777u); }
        }
        unsigned probes = f.models;
        f.entries[1].number = "119";
        query(&f, H2_PAL_OK, 64u, 2u);
        assert(f.models == probes); /* Family cached, number table queried again. */
        finish(&f);
    }
    const char *unknown[] = {"EC800MX", "EC25X", "EC200U"};
    for (size_t i = 0u; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        fixture_t f;
        init(&f, unknown[i], H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
        query(&f, H2_PAL_ERR_UNSUPPORTED, 64u, 0u);
        assert(calls(&f, "AT+CPB") == 0u && calls(&f, "AT+QECCNUM") == 0u);
        finish(&f);
    }
    fixture_t f;
    init(&f, "EC25-E", H2_QUECTEL_MODEM_PROFILE_EC800M_UART);
    query(&f, H2_PAL_ERR_UNSUPPORTED, 64u, 0u);
    finish(&f);
    init(&f, "EC25-E", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    query(&f, H2_PAL_OK, 64u, 2u);
    assert(calls(&f, "AT+CPB") == 0u && calls(&f, "AT+QECCNUM?") == 1u);
    finish(&f);
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.fail_command = "AT+CGMM";
    f.fail_reply = "ERROR\r\n";
    query(&f, H2_PAL_ERR_IO, 64u, 0u);
    assert(!f.modem.model_checked && calls(&f, "AT+CPB") == 0u);
    f.fail_command = NULL;
    query(&f, H2_PAL_OK, 64u, 2u);
    assert(f.models == 2u);
    finish(&f);
}

static void test_phonebook_variants(void) {
    fixture_t f;
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    strcpy(f.storage, "EN");
    query(&f, H2_PAL_OK, 64u, 2u);
    assert(calls(&f, "AT+CPBS=\"") == 0u);
    finish(&f);
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.used = 0u;
    f.entry_count = 0u;
    query(&f, H2_PAL_OK, 64u, 0u);
    assert(calls(&f, "AT+CPBR") == 0u && strcmp(f.storage, "SM") == 0);
    finish(&f);
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.ranges = "+CPBR: (1,3,8-11),20,14";
    f.total = 6u;
    query(&f, H2_PAL_OK, 64u, 2u);
    assert(calls(&f, "AT+CPBR=") == 4u); /* Test form plus three sparse ranges. */
    finish(&f);
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.ranges = "+CPBR: (3,1-2,2-3),20,14";
    query(&f, H2_PAL_OK, 64u, 2u);
    assert(calls(&f, "AT+CPBR=") == 2u); /* One normalized read plus test. */
    finish(&f);
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.used = f.total = 40u;
    f.entry_count = 40u;
    f.ranges = "+CPBR: (1-40),20,14";
    for (unsigned i = 0u; i < 40u; i++) { f.entries[i] = (phonebook_entry_t){i + 1u, "112"}; }
    query(&f, H2_PAL_OK, 64u, 40u);
    assert(calls(&f, "AT+CPBR=") == 6u);
    query(&f, H2_PAL_ERR_TRUNCATED, 39u, 0u);
    assert(strcmp(f.storage, "SM") == 0);
    finish(&f);
}

static void test_failures_and_restore(void) {
    fixture_t f;
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.supported = "+CPBS: (\"SM\",\"ME\")";
    query(&f, H2_PAL_ERR_UNSUPPORTED, 64u, 0u);
    assert(calls(&f, "AT+CPBS=\"") == 0u);
    finish(&f);
    const char *stages[] = {"AT+CPBS=?", "AT+CPBS?", "AT+CPBS=\"EN\"", "AT+CPBR=?", "AT+CPBR=1,3"};
    for (size_t i = 0u; i < sizeof(stages) / sizeof(stages[0]); i++) {
        init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
        f.fail_command = stages[i];
        f.failure = H2_PAL_ERR_TIMEOUT;
        f.apply_failed_selection = 1;
        query(&f, H2_PAL_ERR_TIMEOUT, 64u, 0u);
        assert(strcmp(f.storage, "SM") == 0 && !f.modem.phonebook_restore_pending);
        finish(&f);
    }
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.fail_command = "AT+CPBS=?";
    f.fail_reply = "ERROR\r\n";
    query(&f, H2_PAL_ERR_UNSUPPORTED, 64u, 0u);
    finish(&f);
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.fail_command = "AT+CPBS?";
    f.fail_reply = "+CME ERROR: 10\r\n";
    /* First absence invalidates the SIM epoch; a stable absent SIM reports
     * availability on the next attempt, without a readiness gate. */
    query(&f, H2_PAL_ERR_INVALID_STATE, 64u, 0u);
    f.fail_seen = 0u;
    query(&f, H2_PAL_ERR_UNAVAILABLE, 64u, 0u);
    finish(&f);
    const char *malformed[] = {
        "+CPBR: 1,\"x\",129,\"name\"\r\nOK\r\n",
        "+CPBR: 4,\"112\",129,\"name\"\r\nOK\r\n",
        "+CPBR: 1,\"112\",129,\"name\"\r\n+CPBR: 1,\"120\",129,\"name\"\r\nOK\r\n",
    };
    for (size_t i = 0u; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
        f.read_reply = malformed[i];
        query(&f, H2_PAL_ERR_FORMAT, 64u, 0u);
        assert(strcmp(f.storage, "SM") == 0);
        finish(&f);
    }
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.read_reply = "OK\r\n";
    query(&f, H2_PAL_ERR_TRUNCATED, 64u, 0u);
    finish(&f);
    const char *bad_ranges[] = {
        "+CPBR: (3-1),20,14", "+CPBR: (1-5000),20,14",
        "+CPBR: (1-4294967295),20,14", "+CPBR: (1-3),0,14",
    };
    for (size_t i = 0u; i < sizeof(bad_ranges) / sizeof(bad_ranges[0]); i++) {
        init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
        f.ranges = bad_ranges[i];
        query(&f, i == 1u ? H2_PAL_ERR_TRUNCATED : H2_PAL_ERR_FORMAT, 64u, 0u);
        assert(strcmp(f.storage, "SM") == 0);
        finish(&f);
    }
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.fail_command = "AT+CPBS=\"SM\"";
    f.fail_reply = "ERROR\r\n";
    query(&f, H2_PAL_ERR_IO, 64u, 0u);
    assert(f.modem.phonebook_restore_pending && strcmp(f.storage, "EN") == 0);
    f.fail_command = NULL;
    unsigned before = f.count;
    query(&f, H2_PAL_OK, 64u, 2u);
    assert(strcmp(f.commands[before], "AT+CPBS=\"SM\"") == 0);
    finish(&f);

    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.fail_command = "AT+CPBS=\"SM\"";
    f.fail_reply = "ERROR\r\n";
    query(&f, H2_PAL_ERR_IO, 64u, 0u);
    f.fail_seen = 0u;
    assert(h2_pal_modem_close(&f.modem.platform, 777u) == H2_PAL_ERR_IO);
    assert(!f.modem.opened && !f.modem.phonebook_restore_pending);
    assert(calls(&f, "AT+QPPPDROP") != 0u);
    assert(h2_pal_modem_close(&f.modem.platform, 777u) == H2_PAL_OK);
    finish(&f);
}

static void test_invalidation(void) {
    const char *stages[] = {"AT+CPBS=?", "AT+CPBS=\"EN\"", "AT+CPBR=1,3", "AT+CPBS=\"SM\""};
    for (size_t i = 0u; i < sizeof(stages) / sizeof(stages[0]); i++) {
        fixture_t f;
        init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
        f.invalidate_command = stages[i];
        f.invalidate = i % 2u ? 2 : 1;
        query(&f, H2_PAL_ERR_INVALID_STATE, 64u, 0u);
        query(&f, H2_PAL_OK, 64u, 2u);
        assert(strcmp(f.storage, "SM") == 0);
        finish(&f);
    }
    fixture_t f;
    init(&f, "EC800M", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    f.read_reply = "+CPBR: 1,\"110\",129,\"name\"\r\nRDY\r\n+CPBR: 3,\"120\",129,\"name\"\r\nOK\r\n";
    query(&f, H2_PAL_ERR_INVALID_STATE, 64u, 0u);
    assert(f.modem.phonebook_restore_pending);
    f.read_reply = NULL;
    query(&f, H2_PAL_OK, 64u, 2u);
    finish(&f);
}

static void test_quality(void) {
    fixture_t f;
    init(&f, "EC800M-CN", H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED);
    const char *replies[] = {
        "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,123,1,100,3,5,5,10,-96,-10,-73,10,1\r\nOK\r\n",
        "+QENG: \"servingcell\",\"CONNECT\",\"LTE\",\"FDD\",460,00,123,1,100,3,5,5,10,-95,-10,-73,10,1\r\nOK\r\n",
        "+QENG: \"servingcell\",\"LIMSRV\",\"LTE\",\"FDD\",460,00,123,1,100,3,5,5,10,-94,-10,-73,10,1\r\nOK\r\n",
        "+QENG: \"servingcell\",\"SEARCH\"\r\nOK\r\n",
        "+QENG: \"neighbourcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,123,1,100,3,5,5,10,-96,-10,-73,10,1\r\nOK\r\n",
        "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,123,1,100,3,5,5,10,-999999999999999999999,-10,-73,10,1\r\nOK\r\n",
        "ERROR\r\n",
    };
    for (size_t i = 0u; i < sizeof(replies) / sizeof(replies[0]); i++) {
        f.quality_reply = replies[i];
        h2_pal_modem_signal_t signal;
        assert(h2_pal_modem_get_signal(&f.modem.platform, &signal) == H2_PAL_OK);
        assert(signal.rssi_valid && signal.rssi_dbm == -73);
        assert(signal.rsrp_valid == (i < 3u));
        assert(signal.rsrp_dbm == (i < 3u ? -96 + (int)i : 0));
        assert_released(&f);
    }
    assert(calls(&f, "AT+QCSQ") == 0u);
    h2_pal_modem_cell_location_t location;
    f.modem.data_status.state = H2_PAL_MODEM_DATA_OPEN;
    assert(h2_pal_modem_cell_locate(&f.modem.platform, 0u, &location) == H2_PAL_ERR_UNSUPPORTED);
    assert(calls(&f, "AT+QLBS") == 0u);
    finish(&f);
}

static void test_shared_gnss_and_power(void) {
    fixture_t f;
    init_with_power(&f, "EC800M-CN", H2_QUECTEL_MODEM_PROFILE_EC800M_UART, 1);
    assert(f.modem.capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER);
    assert(h2_pal_modem_set_power_policy(&f.modem.platform,
        H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) == H2_PAL_OK);
    assert(f.asleep);
    query(&f, H2_PAL_OK, 64u, 2u);
    assert(f.asleep);
    assert(h2_pal_modem_gnss_start(&f.modem.platform, 777u) == H2_PAL_OK);
    assert(!f.asleep && f.modem.gnss_hold);
    h2_pal_modem_gnss_fix_t fix;
    assert(h2_pal_modem_get_gnss_fix(&f.modem.platform, &fix) == H2_PAL_OK);
    assert(fix.valid && fix.latitude_e7 == 300000000 && fix.longitude_e7 == 1200000000);
    assert(fix.year == 2026 && fix.month == 10 && fix.day == 8 && fix.satellites == 10);
    assert(fix.speed_cm_s == 1000 && fix.course_deg100 == 9000);
    assert(h2_pal_modem_gnss_stop(&f.modem.platform, 777u) == H2_PAL_OK);
    assert(f.asleep && !f.modem.gnss_hold);
    assert_released(&f);
    finish(&f);
}

int main(void) {
    test_missing_terminal_ok();
    test_models();
    test_phonebook_variants();
    test_failures_and_restore();
    test_invalidation();
    test_quality();
    test_shared_gnss_and_power();
    return 0;
}
