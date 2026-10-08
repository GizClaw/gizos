#include "h2_lierda_modem.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_mutex { int locked; };

typedef struct fixture {
    h2_pal_mutex_t mutex;
    h2_lierda_modem_t *modem;
    h2_pal_modem_api_t *api;
    unsigned allocations;
    unsigned opens;
    unsigned closes;
    unsigned commands;
    unsigned dials;
    unsigned data_closes;
    bool fail_alloc;
    bool fail_mutex_destroy;
    h2_pal_result_t open_error;
    h2_pal_result_t close_error;
    h2_pal_result_t dial_error;
    h2_pal_result_t data_close_error;
    h2_pal_result_t command_error;
    const char *error_command;
    const char *override_command;
    const char *override_response;
    bool unterminated;
    h2_pal_modem_data_status_t data;
    h2_pal_modem_apn_config_t dial_apn;
} fixture_t;

static void *allocate(void *user, size_t len) {
    fixture_t *f = user;
    if (f->fail_alloc) return NULL;
    void *p = malloc(len);
    if (p != NULL) ++f->allocations;
    return p;
}

static void release(void *user, void *p) {
    fixture_t *f = user;
    assert(f->allocations != 0u);
    --f->allocations;
    free(p);
}

static h2_pal_result_t mutex_create(
    void *user, const h2_pal_mutex_config_t *config, h2_pal_mutex_t **out_mutex) {
    fixture_t *f = user;
    assert(config->flags == H2_PAL_MUTEX_FLAG_NONE);
    *out_mutex = &f->mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    fixture_t *f = user;
    assert(mutex == &f->mutex && !mutex->locked);
    return f->fail_mutex_destroy ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    assert(mutex == &((fixture_t *)user)->mutex && !mutex->locked);
    mutex->locked = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    assert(mutex == &((fixture_t *)user)->mutex && mutex->locked);
    mutex->locked = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t transport_open(void *user, uint32_t timeout_ms) {
    fixture_t *f = user;
    (void)timeout_ms;
    assert(f->mutex.locked);
    ++f->opens;
    return f->open_error;
}

static h2_pal_result_t transport_close(void *user, uint32_t timeout_ms) {
    fixture_t *f = user;
    (void)timeout_ms;
    assert(f->mutex.locked);
    ++f->closes;
    if (f->close_error == H2_PAL_OK) memset(&f->data, 0, sizeof(f->data));
    return f->close_error;
}

static h2_pal_result_t command(
    void *user, const char *cmd, char *response, size_t capacity, uint32_t timeout_ms) {
    fixture_t *f = user;
    assert(f->mutex.locked && f->data.state == H2_PAL_MODEM_DATA_CLOSED);
    assert(timeout_ms != 0u);
    ++f->commands;
    if (f->error_command != NULL && strcmp(cmd, f->error_command) == 0) {
        return f->command_error;
    }
    const char *reply = "";
    if (f->override_command != NULL && strcmp(cmd, f->override_command) == 0) {
        reply = f->override_response;
        if (f->unterminated) {
            memset(response, 'X', capacity);
            return H2_PAL_OK;
        }
    } else if (strcmp(cmd, "AT+CPIN?") == 0) reply = "+CPIN: READY\r\n";
    else if (strcmp(cmd, "AT+CEREG?") == 0) reply = "+CEREG: 0,1\r\n";
    else if (strcmp(cmd, "AT+CGATT?") == 0) reply = "+CGATT: 1\r\n";
    else if (strcmp(cmd, "AT+CSQ") == 0) reply = "+CSQ: 21,0\r\n";
    else if (strcmp(cmd, "AT+COPS?") == 0) reply = "+COPS: 0,0,\"Test operator\",7";
    else if (strcmp(cmd, "AT+CGMI") == 0) reply = "AT&T test manufacturer";
    else if (strcmp(cmd, "AT+CGMM") == 0) reply = "bounded module model";
    else if (strcmp(cmd, "AT+CGMR") == 0) reply = "test revision";
    else if (strcmp(cmd, "AT+CGSN") == 0) reply = "000000000000001";
    else if (strcmp(cmd, "AT+CIMI") == 0) reply = "001010000000001";
    else if (strcmp(cmd, "AT") != 0 && strcmp(cmd, "ATE0") != 0 &&
             strcmp(cmd, "AT+CMEE=2") != 0 && strncmp(cmd, "AT+CGDCONT=", 11u) != 0) {
        assert(!"unexpected command (including any vendor extension)");
    }
    const size_t len = strlen(reply);
    if (len >= capacity) return H2_PAL_ERR_TRUNCATED;
    memcpy(response, reply, len + 1u);
    return H2_PAL_OK;
}

static h2_pal_result_t dial(
    void *user, const h2_pal_modem_apn_config_t *apn, uint32_t timeout_ms) {
    fixture_t *f = user;
    (void)timeout_ms;
    assert(f->mutex.locked);
    ++f->dials;
    f->dial_apn = *apn;
    f->data.state = f->dial_error == H2_PAL_OK
        ? H2_PAL_MODEM_DATA_OPEN : H2_PAL_MODEM_DATA_OPENING;
    f->data.ip4_valid = f->dial_error == H2_PAL_OK;
    f->data.ip4 = f->dial_error == H2_PAL_OK ? 0x01020304u : 0u;
    return f->dial_error;
}

static h2_pal_result_t data_close(void *user, uint32_t timeout_ms) {
    fixture_t *f = user;
    (void)timeout_ms;
    assert(f->mutex.locked);
    ++f->data_closes;
    if (f->data_close_error == H2_PAL_OK) memset(&f->data, 0, sizeof(f->data));
    else f->data.state = H2_PAL_MODEM_DATA_CLOSING;
    return f->data_close_error;
}

static h2_pal_result_t data_status(void *user, h2_pal_modem_data_status_t *out_status) {
    fixture_t *f = user;
    assert(f->mutex.locked);
    *out_status = f->data;
    return H2_PAL_OK;
}

static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate, .free = release};
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = mutex_create, .destroy_mutex = mutex_destroy,
    .lock_mutex = mutex_lock, .unlock_mutex = mutex_unlock,
};

static h2_lierda_modem_config_t configuration(
    fixture_t *f, const h2_pal_mem_api_t *mem, const h2_pal_sync_api_t *sync) {
    const h2_lierda_modem_config_t config = {
        .model = H2_LIERDA_MODEM_MODEL_NT26KCNB20NNC,
        .transport = {.user = f, .open = transport_open, .close = transport_close,
            .command = command, .data_open = dial, .data_close = data_close,
            .data_status = data_status},
        .allocator = mem, .sync_api = sync, .apn = {.apn = "test.apn"},
    };
    return config;
}

static void create(fixture_t *f, h2_pal_mem_api_t *mem, h2_pal_sync_api_t *sync) {
    *mem = (h2_pal_mem_api_t){.user = f, .vtable = &memory_vtable};
    *sync = (h2_pal_sync_api_t){.user = f, .vtable = &sync_vtable};
    h2_lierda_modem_config_t config = configuration(f, mem, sync);
    assert(h2_lierda_modem_create(&config, &f->modem) == H2_PAL_OK);
    f->api = h2_lierda_modem_api(f->modem);
    memset(&config, 0, sizeof(config)); /* config is not retained */
}

static void destroy(fixture_t *f) {
    assert(h2_lierda_modem_destroy(f->modem) == H2_PAL_OK);
    assert(f->allocations == 0u && !f->mutex.locked);
}

static void lifecycle_and_data(void) {
    fixture_t f = {0};
    h2_pal_mem_api_t mem;
    h2_pal_sync_api_t sync;
    create(&f, &mem, &sync);
    assert(f.opens == 0u && f.commands == 0u);
    uint32_t caps = 0u;
    assert(h2_pal_modem_get_capabilities(f.api, &caps) == H2_PAL_OK);
    assert(caps == H2_PAL_MODEM_CAPABILITY_DATA);
    h2_pal_modem_signal_t signal;
    memset(&signal, 0xff, sizeof(signal));
    assert(h2_pal_modem_get_signal(f.api, &signal) == H2_PAL_ERR_CLOSED);
    assert(signal.rssi_valid == 0u && signal.rssi_dbm == 0);
    assert(h2_pal_modem_open(f.api, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_open(f.api, 0u) == H2_PAL_OK && f.opens == 1u);
    h2_pal_modem_identity_t identity;
    assert(h2_pal_modem_get_identity(f.api, &identity) == H2_PAL_OK);
    assert(strcmp(identity.model, "bounded module model") == 0);
    assert(strcmp(identity.manufacturer, "AT&T test manufacturer") == 0);
    h2_pal_modem_operator_t oper;
    assert(h2_pal_modem_get_operator(f.api, &oper) == H2_PAL_OK);
    assert(strcmp(oper.name, "Test operator") == 0 && oper.rat == H2_PAL_MODEM_RAT_LTE);
    h2_pal_modem_apn_config_t apn = {.apn = "other.apn", .username = "user", .password = "fixture"};
    assert(h2_pal_modem_set_apn(f.api, &apn) == H2_PAL_OK);
    memset(&apn, 0, sizeof(apn));
    assert(h2_pal_modem_data_open(f.api, 0u) == H2_PAL_OK);
    assert(strcmp(f.dial_apn.apn, "other.apn") == 0);
    assert(strcmp(f.dial_apn.password, "fixture") == 0);
    const unsigned before = f.commands;
    assert(h2_pal_modem_get_signal(f.api, &signal) == H2_PAL_ERR_BUSY);
    assert(h2_pal_modem_get_identity(f.api, &identity) == H2_PAL_ERR_BUSY);
    apn = (h2_pal_modem_apn_config_t){.apn = "blocked"};
    assert(h2_pal_modem_set_apn(f.api, &apn) == H2_PAL_ERR_BUSY);
    assert(f.commands == before);
    h2_pal_modem_data_status_t data;
    assert(h2_pal_modem_get_data_status(f.api, &data) == H2_PAL_OK);
    assert(data.state == H2_PAL_MODEM_DATA_OPEN && data.ip4_valid);
    f.data_close_error = H2_PAL_ERR_TIMEOUT;
    assert(h2_pal_modem_data_close(f.api, 5u) == H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_modem_get_signal(f.api, &signal) == H2_PAL_ERR_BUSY);
    f.data_close_error = H2_PAL_OK;
    assert(h2_pal_modem_data_close(f.api, 5u) == H2_PAL_OK);
    assert(h2_pal_modem_get_signal(f.api, &signal) == H2_PAL_OK);
    assert(signal.rssi_valid && signal.rssi_dbm == -71 && !signal.rsrp_valid);
    assert(h2_pal_modem_gnss_start(f.api, 1u) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_call_hangup(f.api, 1u) == H2_PAL_ERR_UNSUPPORTED);
    destroy(&f);
}

static void identity_and_signal_failures(void) {
    fixture_t f = {0};
    h2_pal_mem_api_t mem;
    h2_pal_sync_api_t sync;
    create(&f, &mem, &sync);
    assert(h2_pal_modem_open(f.api, 0u) == H2_PAL_OK);
    h2_pal_modem_identity_t identity;
    f.override_command = "AT+CIMI";
    f.override_response = "001010000000001\r\n001010000000001\r\n";
    assert(h2_pal_modem_get_identity(f.api, &identity) == H2_PAL_ERR_FORMAT);
    assert(identity.imei[0] == '\0' && identity.imsi[0] == '\0');
    char long_reply[100];
    memset(long_reply, '1', sizeof(long_reply) - 1u);
    long_reply[sizeof(long_reply) - 1u] = '\0';
    f.override_response = long_reply;
    assert(h2_pal_modem_get_identity(f.api, &identity) == H2_PAL_ERR_TRUNCATED);
    assert(identity.model[0] == '\0');
    f.unterminated = true;
    assert(h2_pal_modem_get_identity(f.api, &identity) == H2_PAL_ERR_TRUNCATED);
    f.unterminated = false;
    f.error_command = "AT+CIMI";
    f.command_error = H2_PAL_ERR_TIMEOUT;
    assert(h2_pal_modem_get_identity(f.api, &identity) == H2_PAL_ERR_TIMEOUT);
    f.error_command = NULL;
    f.override_command = "AT+CSQ";
    const char *unknown[] = {"+CSQ: 99,99", "+CSQ: 32,0", "+CSQ: 100,0"};
    h2_pal_modem_signal_t signal;
    for (size_t i = 0u; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        f.override_response = unknown[i];
        assert(h2_pal_modem_get_signal(f.api, &signal) == H2_PAL_OK);
        assert(!signal.rssi_valid && signal.rssi_dbm == 0 && !signal.rsrp_valid);
    }
    f.override_response = "+CSQ: 31,8";
    assert(h2_pal_modem_get_signal(f.api, &signal) == H2_PAL_ERR_FORMAT);
    assert(signal.ber == 0);
    f.override_response = "+CSQ: 9999999999999999999999,0";
    assert(h2_pal_modem_get_signal(f.api, &signal) == H2_PAL_ERR_FORMAT);
    destroy(&f);
}

static void sim_and_registration_gates(void) {
    fixture_t f = {0};
    h2_pal_mem_api_t mem;
    h2_pal_sync_api_t sync;
    create(&f, &mem, &sync);
    assert(h2_pal_modem_open(f.api, 0u) == H2_PAL_OK);
    f.override_command = "AT+CPIN?";
    f.override_response = "+CPIN: SIM PIN";
    assert(h2_pal_modem_data_open(f.api, 0u) == H2_PAL_ERR_INVALID_STATE);
    assert(f.dials == 0u);
    f.override_command = "AT+CEREG?";
    f.override_response = "+CEREG: 0,2";
    assert(h2_pal_modem_data_open(f.api, 0u) == H2_PAL_ERR_INVALID_STATE);
    f.override_response = "+CEREG: 1"; /* URC is not a solicited query */
    assert(h2_pal_modem_data_open(f.api, 0u) == H2_PAL_ERR_FORMAT);
    f.override_response = "+CEREG: 0,5";
    assert(h2_pal_modem_data_open(f.api, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_data_close(f.api, 0u) == H2_PAL_OK);
    f.override_command = "AT+CGATT?";
    f.override_response = "+CGATT: 2";
    assert(h2_pal_modem_data_open(f.api, 0u) == H2_PAL_ERR_FORMAT);
    f.override_command = NULL;
    f.dial_error = H2_PAL_ERR_TIMEOUT;
    assert(h2_pal_modem_data_open(f.api, 1u) == H2_PAL_ERR_TIMEOUT);
    const unsigned commands_before = f.commands;
    h2_pal_modem_status_t status;
    assert(h2_pal_modem_get_status(f.api, &status) == H2_PAL_ERR_BUSY);
    assert(f.commands == commands_before);
    /* Whole close can recover without graceful data close. */
    assert(h2_pal_modem_close(f.api, 0u) == H2_PAL_OK);
    assert(f.data.state == H2_PAL_MODEM_DATA_CLOSED);
    destroy(&f);
}

static void configuration_and_cleanup_failures(void) {
    fixture_t f = {0};
    h2_pal_mem_api_t mem = {.user = &f, .vtable = &memory_vtable};
    h2_pal_sync_api_t sync = {.user = &f, .vtable = &sync_vtable};
    h2_lierda_modem_config_t config = configuration(&f, &mem, &sync);
    config.model = 0;
    h2_lierda_modem_t *out = (h2_lierda_modem_t *)&f;
    assert(h2_lierda_modem_create(&config, &out) == H2_PAL_ERR_INVALID_ARG && out == NULL);
    config = configuration(&f, &mem, &sync);
    memset(config.apn.apn, 'a', sizeof(config.apn.apn));
    assert(h2_lierda_modem_create(&config, &out) == H2_PAL_ERR_INVALID_ARG);
    config = configuration(&f, &mem, &sync);
    strcpy(config.apn.apn, "apn\"\rAT");
    assert(h2_lierda_modem_create(&config, &out) == H2_PAL_ERR_INVALID_ARG);
    config = configuration(&f, &mem, &sync);
    f.fail_alloc = true;
    assert(h2_lierda_modem_create(&config, &out) == H2_PAL_ERR_NO_MEMORY);
    f.fail_alloc = false;
    create(&f, &mem, &sync);
    f.error_command = "ATE0";
    f.command_error = H2_PAL_ERR_TIMEOUT;
    f.close_error = H2_PAL_ERR_IO;
    assert(h2_pal_modem_open(f.api, 0u) == H2_PAL_ERR_IO);
    assert(h2_pal_modem_open(f.api, 0u) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_lierda_modem_destroy(f.modem) == H2_PAL_ERR_IO && f.allocations == 1u);
    f.close_error = H2_PAL_OK;
    f.fail_mutex_destroy = true;
    assert(h2_lierda_modem_destroy(f.modem) == H2_PAL_ERR_IO && f.allocations == 1u);
    f.fail_mutex_destroy = false;
    destroy(&f);
}

int main(void) {
    lifecycle_and_data();
    identity_and_signal_failures();
    sim_and_registration_gates();
    configuration_and_cleanup_failures();
    puts("Lierda data-only protocol and lifecycle tests passed");
    return 0;
}
