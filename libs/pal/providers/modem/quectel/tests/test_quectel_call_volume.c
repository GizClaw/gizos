#include "h2_quectel_modem.h"
#include "h2_simcom_modem.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Count recursive acquisitions as well as operation_depth, so an unbalanced
 * lock cannot be hidden by a subsequent call on the same task. */
struct h2_pal_mutex { unsigned depth; };
static h2_pal_result_t create_mutex(void *user, const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out) {
    (void)user;
    assert(config->flags == H2_PAL_MUTEX_FLAG_RECURSIVE);
    *out = calloc(1, sizeof(**out));
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

typedef struct {
    h2_quectel_modem_t modem;
    const char *range;
    const char *value;
    const char *fail_command;
    h2_pal_result_t failure;
    unsigned probes, reads, sets, prepares, wakes;
    int level;
    int asleep;
} fixture_t;

static h2_pal_result_t gate(void *user, int allow_sleep) {
    fixture_t *f = user;
    f->asleep = allow_sleep;
    if (!allow_sleep) { f->wakes++; }
    return H2_PAL_OK;
}

static h2_pal_result_t command(void *user, const char *cmd, char *response,
    size_t size, uint32_t timeout_ms) {
    fixture_t *f = user;
    (void)timeout_ms;
    assert(f->modem.operation_lock->depth > 0u);
    assert(f->modem.lock->depth == 0u);
    assert(!f->asleep);
    if (f->fail_command != NULL && strcmp(cmd, f->fail_command) == 0) {
        return f->failure;
    }
    const char *text = "OK\r\n";
    if (strcmp(cmd, "AT+CLVL=?") == 0) {
        f->probes++;
        text = f->range;
    } else if (strcmp(cmd, "AT+CLVL?") == 0) {
        f->reads++;
        text = f->value;
    } else if (strncmp(cmd, "AT+CLVL=", 8u) == 0) {
        f->sets++;
        assert(sscanf(cmd, "AT+CLVL=%d", &f->level) == 1);
    } else if (strcmp(cmd, "AT") == 0) {
        f->prepares++;
    } else if (strcmp(cmd, "AT+CGMM") == 0) {
        text = "EC25\r\nOK\r\n";
    }
    assert(strlen(text) < size);
    memcpy(response, text, strlen(text) + 1u);
    return H2_PAL_OK;
}

static void released(fixture_t *f) {
    assert(f->modem.operation_depth == 0u);
    assert(f->modem.operation_lock->depth == 0u);
    assert(f->modem.lock->depth == 0u);
}
static void init(fixture_t *f, const char *range, int low_power) {
    memset(f, 0, sizeof(*f));
    f->range = range;
    f->value = "+CLVL: 0\r\nOK\r\n";
    const h2_quectel_modem_config_t config = {
        .command = command, .transport_user = f, .sync_api = &sync_api,
        .profile = low_power ? H2_QUECTEL_MODEM_PROFILE_EC25_UART : 0,
        .sleep_gate = low_power ? gate : NULL,
    };
    assert(h2_quectel_modem_init(&f->modem, &config) == H2_PAL_OK);
    uint32_t capabilities = 0;
    assert(h2_pal_modem_get_capabilities(&f->modem.platform, &capabilities) == H2_PAL_OK);
    assert(capabilities & H2_PAL_MODEM_CAPABILITY_CALL_VOLUME);
    assert(h2_pal_modem_open(&f->modem.platform, 1000u) == H2_PAL_OK);
    assert(f->probes == 0u);
    released(f);
}
static void set(fixture_t *f, uint32_t percent, int level) {
    const uint8_t hold = f->modem.call_hold;
    assert(h2_pal_modem_set_call_volume(&f->modem.platform, percent) == H2_PAL_OK);
    assert(f->level == level);
    assert(f->modem.call_hold == hold);
    released(f);
}
static void get(fixture_t *f, const char *reply, uint32_t expected) {
    f->value = reply;
    uint32_t percent = 999u;
    const uint8_t hold = f->modem.call_hold;
    assert(h2_pal_modem_get_call_volume(&f->modem.platform, &percent) == H2_PAL_OK);
    assert(percent == expected);
    assert(f->modem.call_hold == hold);
    released(f);
}
static void finish(fixture_t *f) {
    assert(h2_quectel_modem_deinit(&f->modem) == H2_PAL_OK);
}

int main(void) {
    uint32_t unsupported_percent = 777, capabilities = 0;
    const h2_pal_modem_api_t *unsupported = h2_pal_unsupported_modem_api();
    assert(h2_pal_modem_set_call_volume(unsupported, 50) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_get_call_volume(unsupported, &unsupported_percent) == H2_PAL_ERR_UNSUPPORTED);
    h2_simcom_modem_t simcom;
    const h2_simcom_modem_config_t simcom_config = {.command = command};
    assert(h2_simcom_modem_init(&simcom, &simcom_config) == H2_PAL_OK);
    assert(h2_pal_modem_get_capabilities(&simcom.platform, &capabilities) == H2_PAL_OK);
    assert((capabilities & H2_PAL_MODEM_CAPABILITY_CALL_VOLUME) == 0u);
    assert(h2_pal_modem_set_call_volume(&simcom.platform, 50) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_get_call_volume(&simcom.platform, &unsupported_percent) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_simcom_modem_deinit(&simcom) == H2_PAL_OK);
    fixture_t f;
    init(&f, "+CLVL: (0-9)\r\nOK\r\n", 0);
    set(&f, 0, 0); set(&f, 50, 5); set(&f, 100, 9);
    get(&f, "+CLVL: 5\r\nOK\r\n", 56);
    get(&f, "+CLVL: -1\r\nOK\r\n", 0);
    get(&f, "+CLVL: 10\r\nOK\r\n", 100);
    assert(f.probes == 1u);
    assert(h2_pal_modem_open(&f.modem.platform, 1000u) == H2_PAL_OK);
    set(&f, 50, 5);
    assert(f.probes == 1u);
    assert(h2_pal_modem_close(&f.modem.platform, 1000u) == H2_PAL_OK);
    assert(h2_pal_modem_open(&f.modem.platform, 1000u) == H2_PAL_OK);
    f.range = "+CLVL: (0-5)\r\nOK\r\n";
    set(&f, 0, 0); set(&f, 50, 3); set(&f, 100, 5);
    get(&f, "+CLVL: 3\r\nOK\r\n", 60);
    assert(f.probes == 2u);
    const char *bad_values[] = {"OK\r\n", "+CLVL: \r\nOK\r\n",
        "+CLVL: nope\r\nOK\r\n", "+CLVL: 3junk\r\nOK\r\n",
        "+CLVL: 9999999999999999999999\r\nOK\r\n"};
    for (size_t i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); i++) {
        f.value = bad_values[i];
        uint32_t percent = 777;
        assert(h2_pal_modem_get_call_volume(&f.modem.platform, &percent) == H2_PAL_ERR_FORMAT);
        assert(percent == 777);
        released(&f);
    }
    finish(&f);

    const char *bad_ranges[] = {"OK\r\n", "+CLVL: ()\r\nOK\r\n",
        "+CLVL: garbage\r\nOK\r\n", "+CLVL: (9-0)\r\nOK\r\n",
        "+CLVL: (5-5)\r\nOK\r\n", "+CLVL: (0-9\r\nOK\r\n",
        "+CLVL: (0-9)junk\r\nOK\r\n", "+CLVL: (-1-9)\r\nOK\r\n",
        "+CLVL: (0-99999999999999999999)\r\nOK\r\n"};
    for (size_t i = 0; i < sizeof(bad_ranges) / sizeof(bad_ranges[0]); i++) {
        init(&f, bad_ranges[i], 0);
        get(&f, "+CLVL: 3\r\nOK\r\n", 60);
        set(&f, 0, 0); set(&f, 50, 3); set(&f, 100, 5);
        assert(f.probes == 1u);
        finish(&f);
    }
    init(&f, "+CLVL: (2-10)\r\nOK\r\n", 0);
    set(&f, 0, 2); set(&f, 50, 6); set(&f, 100, 10);
    get(&f, "+CLVL: 3\r\nOK\r\n", 13);
    get(&f, "+CLVL: 1\r\nOK\r\n", 0);
    finish(&f);

    const h2_pal_result_t failures[] = {H2_PAL_ERR_TIMEOUT, H2_PAL_ERR_IO, H2_PAL_ERR_UNSUPPORTED};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        init(&f, "+CLVL: (0-9)\r\nOK\r\n", 0);
        f.failure = failures[i];
        const char *commands[] = {"AT+CLVL=?", "AT+CLVL=5", "AT+CLVL?"};
        for (size_t j = 0; j < 3; j++) {
            f.fail_command = commands[j];
            uint32_t percent = 777;
            if (j != 2) {
                assert(h2_pal_modem_set_call_volume(&f.modem.platform, 50) == f.failure);
                released(&f);
            }
            if (j != 1) {
                assert(h2_pal_modem_get_call_volume(&f.modem.platform, &percent) == f.failure);
                assert(percent == 777);
                released(&f);
            }
        }
        f.fail_command = NULL;
        set(&f, 50, 5);
        finish(&f);
    }

    init(&f, "+CLVL: (0-9)\r\nOK\r\n", 1);
    for (uint8_t hold = 0; hold <= 1; hold++) {
        f.modem.call_hold = hold;
        f.modem.prepared = 0;
        unsigned prepares = f.prepares;
        unsigned wakes = f.wakes;
        f.asleep = 1;
        set(&f, 50, 5);
        assert(f.prepares == prepares + 1 && f.wakes > wakes);
        f.modem.prepared = 0;
        prepares = f.prepares;
        get(&f, "+CLVL: 5\r\nOK\r\n", 56);
        assert(f.prepares == prepares + 1);
    }
    f.modem.call_hold = 0;
    f.modem.prepared = 0;
    f.fail_command = "AT";
    f.failure = H2_PAL_ERR_TIMEOUT;
    uint32_t percent;
    assert(h2_pal_modem_set_call_volume(&f.modem.platform, 50) == f.failure);
    released(&f);
    assert(h2_pal_modem_get_call_volume(&f.modem.platform, &percent) == f.failure);
    released(&f);
    f.fail_command = NULL;
    assert(h2_pal_modem_close(&f.modem.platform, 1000) == H2_PAL_OK);
    unsigned probes = f.probes;
    assert(h2_pal_modem_set_call_volume(&f.modem.platform, 50) == H2_PAL_ERR_CLOSED);
    released(&f);
    assert(h2_pal_modem_get_call_volume(&f.modem.platform, &percent) == H2_PAL_ERR_CLOSED);
    released(&f);
    assert(f.probes == probes);
    finish(&f);
    return 0;
}
