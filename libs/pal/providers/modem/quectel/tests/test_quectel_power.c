#define _POSIX_C_SOURCE 200809L
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_quectel_modem.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_mutex {
    pthread_mutex_t mutex;
};

typedef struct fixture {
    h2_quectel_modem_t modem;
    unsigned commands;
    unsigned sleep_count;
    unsigned wake_count;
    unsigned sim_events;
    unsigned invalidations;
    unsigned deinit_count;
    int asleep_allowed;
    int fail_wake;
    int fail_sleep;
    int fail_deinit;
    int remove_during_dial;
    int sim_level;
    const char *model;
    const char *fail_command;
    const char *cpin;
    h2_pal_modem_sim_state_t last_sim;
    pthread_mutex_t barrier;
    pthread_cond_t condition;
    int block_command;
    int command_entered;
    int release_command;
} fixture_t;

static h2_pal_result_t create_mutex(void *user, const h2_pal_mutex_config_t *config,
                                    h2_pal_mutex_t **out) {
    (void)user;
    assert(config->flags == H2_PAL_MUTEX_FLAG_RECURSIVE);
    *out = calloc(1, sizeof(**out));
    assert(*out != NULL);
    pthread_mutexattr_t attr;
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
    assert(pthread_mutex_init(&(*out)->mutex, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t destroy_mutex(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_destroy(&mutex->mutex) == 0);
    free(mutex);
    return H2_PAL_OK;
}
static h2_pal_result_t lock_mutex(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_lock(&mutex->mutex) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t unlock_mutex(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_unlock(&mutex->mutex) == 0);
    return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = create_mutex,
    .destroy_mutex = destroy_mutex,
    .lock_mutex = lock_mutex,
    .unlock_mutex = unlock_mutex,
};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};

static h2_pal_result_t gate(void *user, int allow_sleep) {
    fixture_t *f = user;
    if (allow_sleep) {
        assert(f->modem.gnss_hold == 0u && f->modem.call_hold == 0u && f->modem.data_hold == 0u);
        f->sleep_count++;
        if (f->fail_sleep) {
            return H2_PAL_ERR_IO;
        }
    } else {
        f->wake_count++;
        if (f->fail_wake) {
            return H2_PAL_ERR_TIMEOUT;
        }
    }
    f->asleep_allowed = allow_sleep;
    return H2_PAL_OK;
}
static void invalidate_data(void *user) {
    fixture_t *f = user;
    f->invalidations++;
}
static h2_pal_result_t deinit_transport(void *user) {
    fixture_t *f = user;
    f->deinit_count++;
    return f->fail_deinit ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static int post_event(void *user, const h2_pal_system_event_t *event, uint32_t timeout_ms) {
    fixture_t *f = user;
    assert(timeout_ms == 0u);
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIM_CHANGED) {
        assert(event->payload_size == sizeof(h2_pal_modem_status_t));
        f->last_sim = ((const h2_pal_modem_status_t *)event->payload)->sim;
        f->sim_events++;
    }
    return H2_PAL_OK;
}
static const h2_pal_system_event_vtable_t event_vtable = {.post = post_event};

static h2_pal_result_t command(void *user, const char *cmd, char *response, size_t size,
                               uint32_t timeout_ms) {
    fixture_t *f = user;
    (void)timeout_ms;
    assert(!f->asleep_allowed);
    f->commands++;
    if (f->fail_command != NULL && strcmp(cmd, f->fail_command) == 0) {
        return H2_PAL_ERR_TIMEOUT;
    }
    const char *text = "OK\r\n";
    if (strcmp(cmd, "AT+CGMM") == 0) {
        text = f->model;
    }
    if (strcmp(cmd, "AT+QSIMDET?") == 0) {
        text = f->sim_level ? "+QSIMDET: 1,1\r\nOK\r\n" : "+QSIMDET: 1,0\r\nOK\r\n";
    }
    if (strcmp(cmd, "AT+CPIN?") == 0) {
        text = f->cpin;
    }
    if (strcmp(cmd, "AT+CSQ") == 0) {
        text = "+CSQ: 20,0\r\nOK\r\n";
    }
    if (strcmp(cmd, "AT+QLBS") == 0) {
        text = "+QLBS: 0,31.0,117.0,120\r\nOK\r\n";
    }
    if (strcmp(cmd, "ATD*99***1#") == 0 && f->remove_during_dial) {
        text = "+QSIMSTAT: 1,0\r\nCONNECT\r\n";
    }
    if (strcmp(cmd, "AT+QLBS") == 0) {
        assert(pthread_mutex_lock(&f->barrier) == 0);
        if (f->block_command) {
            f->command_entered = 1;
            assert(pthread_cond_broadcast(&f->condition) == 0);
            while (!f->release_command) {
                assert(pthread_cond_wait(&f->condition, &f->barrier) == 0);
            }
        }
        assert(pthread_mutex_unlock(&f->barrier) == 0);
    }
    assert(strlen(text) < size);
    strcpy(response, text);
    return H2_PAL_OK;
}
static void init_fixture(fixture_t *f, h2_pal_system_event_api_t *events, int hotplug) {
    memset(f, 0, sizeof(*f));
    f->model = "EC25-E\r\nOK\r\n";
    f->cpin = "+CPIN: READY\r\nOK\r\n";
    assert(pthread_mutex_init(&f->barrier, NULL) == 0);
    assert(pthread_cond_init(&f->condition, NULL) == 0);
    events->user = f;
    events->vtable = &event_vtable;
    h2_quectel_modem_config_t config = {
        .transport_user = f,
        .command = command,
        .deinit = deinit_transport,
        .sync_api = &sync_api,
        .system_events = events,
        .profile = H2_QUECTEL_MODEM_PROFILE_EC25_UART,
        .sleep_gate = gate,
        .sim_hotplug = (uint8_t)hotplug,
        .invalidate_data = invalidate_data,
        .cell_locate_token = "FAKE_TEST_TOKEN",
    };
    assert(h2_quectel_modem_init(&f->modem, &config) == H2_PAL_OK);
}
static void finish(fixture_t *f) {
    h2_quectel_modem_deinit(&f->modem);
    assert(pthread_cond_destroy(&f->condition) == 0);
    assert(pthread_mutex_destroy(&f->barrier) == 0);
}

static void test_policy_and_holds(void) {
    fixture_t f;
    h2_pal_system_event_api_t events;
    init_fixture(&f, &events, 0);
    assert(h2_quectel_post_urc_line(&f.modem, "+CMTI: \"SM\",1") == H2_PAL_OK);
    assert(h2_quectel_post_urc_line(&f.modem, "+CMT: ignored") == H2_PAL_OK);
    assert(f.modem.call_hold == 0u);
    h2_pal_modem_t *api = &f.modem.platform;
    h2_pal_modem_data_status_t data_status;
    assert(h2_pal_modem_get_data_status(api, &data_status) == H2_PAL_ERR_CLOSED);
    assert(f.modem.operation_depth == 0u);
    assert(h2_pal_modem_set_power_policy(api, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_modem_open(api, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_set_power_policy(api, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) == H2_PAL_OK);
    assert(f.asleep_allowed);
    unsigned commands = f.commands, wakes = f.wake_count;
    assert(h2_pal_modem_get_data_status(api, &data_status) == H2_PAL_OK);
    assert(f.commands == commands && f.wake_count == wakes);
    h2_pal_modem_power_status_t status;
    assert(h2_pal_modem_get_power_status(api, &status) == H2_PAL_OK);
    assert(status.policy == H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP);
    assert(status.state == H2_PAL_MODEM_POWER_STATE_UNKNOWN);
    assert(f.commands == commands && f.wake_count == wakes);
    assert(h2_pal_modem_gnss_start(api, 0u) == H2_PAL_OK);
    assert(!f.asleep_allowed);
    assert(h2_pal_modem_set_power_policy(api, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) == H2_PAL_OK);
    assert(!f.asleep_allowed);
    h2_pal_modem_signal_t signal;
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_OK);
    assert(!f.asleep_allowed);
    f.fail_command = "AT+QGPSEND";
    assert(h2_pal_modem_gnss_stop(api, 0u) == H2_PAL_ERR_TIMEOUT);
    assert(!f.asleep_allowed && f.modem.gnss_hold);
    f.fail_command = NULL;
    assert(h2_pal_modem_gnss_stop(api, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_set_power_policy(api, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) == H2_PAL_OK);
    assert(f.asleep_allowed);
    h2_quectel_handle_urc_line(&f.modem, "RING");
    assert(!f.asleep_allowed && f.modem.call_hold);
    assert(h2_pal_modem_call_answer(api, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_call_hangup(api, 0u) == H2_PAL_OK);
    assert(f.asleep_allowed);
    assert(h2_quectel_modem_dial_ppp(&f.modem) == H2_PAL_OK);
    assert(!f.asleep_allowed && f.modem.data_hold);
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_OK);
    assert(!f.asleep_allowed);
    assert(h2_quectel_modem_drop_ppp(&f.modem) == H2_PAL_OK);
    assert(f.asleep_allowed);
    f.fail_wake = 1;
    commands = f.commands;
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_ERR_TIMEOUT);
    assert(f.commands == commands);
    f.fail_wake = 0;
    assert(h2_pal_modem_set_power_policy(api, H2_PAL_MODEM_POWER_POLICY_ACTIVE) == H2_PAL_OK);
    f.fail_sleep = 1;
    assert(h2_pal_modem_set_power_policy(api, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
           H2_PAL_ERR_IO);
    assert(h2_pal_modem_get_power_status(api, &status) == H2_PAL_OK);
    assert(status.policy == H2_PAL_MODEM_POWER_POLICY_ACTIVE && !f.asleep_allowed);
    f.fail_sleep = 0;
    assert(h2_pal_modem_set_power_policy(api, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) == H2_PAL_OK);
    f.fail_deinit = 1;
    assert(h2_pal_modem_close(api, 0u) == H2_PAL_ERR_IO);
    assert(f.modem.opened);
    f.fail_deinit = 0;
    assert(h2_pal_modem_close(api, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_ERR_CLOSED);
    assert(h2_pal_modem_open(api, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_get_power_status(api, &status) == H2_PAL_OK);
    assert(status.policy == H2_PAL_MODEM_POWER_POLICY_ACTIVE);
    finish(&f);
}

static void test_close_preserves_failed_sessions(void) {
    const char *failed_commands[] = {"AT+QGPSEND", "ATH", "AT+QPPPDROP"};
    for (size_t i = 0; i < sizeof(failed_commands) / sizeof(failed_commands[0]); ++i) {
        fixture_t f;
        h2_pal_system_event_api_t events;
        init_fixture(&f, &events, 0);
        h2_pal_modem_t *api = &f.modem.platform;
        assert(h2_pal_modem_open(api, 0u) == H2_PAL_OK);
        assert(h2_pal_modem_set_power_policy(api, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
               H2_PAL_OK);
        if (i == 0u) {
            assert(h2_pal_modem_gnss_start(api, 0u) == H2_PAL_OK);
        } else if (i == 1u) {
            h2_quectel_handle_urc_line(&f.modem, "RING");
        } else {
            assert(h2_quectel_modem_dial_ppp(&f.modem) == H2_PAL_OK);
        }
        f.fail_command = failed_commands[i];
        h2_pal_mutex_t *lock = f.modem.lock;
        assert(h2_pal_modem_close(api, 0u) == H2_PAL_ERR_TIMEOUT);
        assert(f.modem.opened && f.modem.lock == lock && f.deinit_count == 0u);
        assert(!f.asleep_allowed);
        assert((i == 0u && f.modem.gnss_hold) || (i == 1u && f.modem.call_hold) ||
               (i == 2u && f.modem.data_hold));
        assert(f.modem.power_policy == H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP);
        h2_quectel_modem_deinit(&f.modem);
        assert(f.modem.opened && f.modem.lock == lock && f.deinit_count == 0u);
        f.fail_command = NULL;
        assert(h2_pal_modem_close(api, 0u) == H2_PAL_OK);
        assert(!f.modem.opened && f.deinit_count == 1u);
        finish(&f);
    }
}

static void test_sim_and_reset(void) {
    fixture_t f;
    h2_pal_system_event_api_t events;
    init_fixture(&f, &events, 1);
    assert(h2_pal_modem_open(&f.modem.platform, 0u) == H2_PAL_OK);
    h2_quectel_handle_urc_line(&f.modem, "+CPIN: READY");
    h2_quectel_handle_urc_line(&f.modem, "+CPIN: READY");
    h2_quectel_handle_urc_line(&f.modem, "+QSIMSTAT: 1,1");
    assert(f.sim_events == 1u && f.last_sim == H2_PAL_MODEM_SIM_STATE_READY);
    assert(h2_quectel_modem_dial_ppp(&f.modem) == H2_PAL_OK);
    f.modem.data_status.ip4_valid = 1u;
    h2_quectel_handle_urc_line(&f.modem, "+QSIMSTAT : 1,0");
    assert(f.last_sim == H2_PAL_MODEM_SIM_STATE_ABSENT && f.invalidations == 1u);
    assert(f.modem.data_status.state == H2_PAL_MODEM_DATA_CLOSED && !f.modem.data_status.ip4_valid);
    h2_quectel_handle_urc_line(&f.modem, "+CPIN: NOT READY");
    h2_quectel_handle_urc_line(&f.modem, "+QSIMSTAT: 1,0");
    assert(f.sim_events == 2u);
    h2_quectel_handle_urc_line(&f.modem, "+QSIMSTAT: 1,1");
    assert(f.last_sim == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
    h2_quectel_handle_urc_line(&f.modem, "+CPIN: SIM PIN");
    assert(f.last_sim == H2_PAL_MODEM_SIM_STATE_LOCKED);
    unsigned count = f.commands;
    assert(h2_quectel_modem_dial_ppp(&f.modem) == H2_PAL_ERR_INVALID_STATE);
    assert(f.commands == count);
    h2_quectel_handle_urc_line(&f.modem, "+CPIN: SIM PUK");
    assert(f.sim_events == 4u);
    h2_quectel_handle_urc_line(&f.modem, "+CPIN: READY");
    f.remove_during_dial = 1;
    assert(h2_quectel_modem_dial_ppp(&f.modem) == H2_PAL_ERR_INVALID_STATE);
    assert(f.modem.data_status.state == H2_PAL_MODEM_DATA_CLOSED && !f.modem.data_hold);
    h2_quectel_handle_urc_line(&f.modem, "+QSIMSTAT: 1,2");
    assert(f.last_sim == H2_PAL_MODEM_SIM_STATE_UNKNOWN);
    count = f.sim_events;
    h2_quectel_handle_urc_line(&f.modem, "+QSIMSTAT: 1,3");
    h2_quectel_handle_urc_line(&f.modem, "+QSIMSTAT: 1,0junk");
    assert(f.sim_events == count);
    assert(h2_pal_modem_set_power_policy(&f.modem.platform, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
           H2_PAL_OK);
    h2_quectel_handle_urc_line(&f.modem, "RDY");
    assert(!f.modem.prepared && !f.modem.power_configured);
    h2_pal_modem_signal_t signal;
    assert(h2_pal_modem_get_signal(&f.modem.platform, &signal) == H2_PAL_OK);
    assert(f.modem.prepared && f.modem.power_configured && f.asleep_allowed);
    finish(&f);
}

static void test_unsupported_and_restart(void) {
    h2_pal_modem_power_status_t status;
    assert(h2_pal_modem_get_power_status(NULL, &status) == H2_PAL_ERR_UNSUPPORTED);
    assert(status.state == H2_PAL_MODEM_POWER_STATE_UNKNOWN);
    assert(h2_pal_modem_set_power_policy(h2_pal_unsupported_modem_api(),
                                         H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_set_power_policy(NULL, (h2_pal_modem_power_policy_t)42) ==
           H2_PAL_ERR_INVALID_ARG);
    fixture_t f;
    h2_pal_system_event_api_t events;
    init_fixture(&f, &events, 0);
    h2_quectel_modem_config_t config = f.modem.config;
    h2_quectel_modem_deinit(&f.modem);
    config.sleep_gate = NULL;
    config.capabilities = H2_PAL_MODEM_CAPABILITY_LOW_POWER;
    assert(h2_quectel_modem_init(&f.modem, &config) == H2_PAL_OK);
    uint32_t capabilities = 0u;
    assert(h2_pal_modem_get_capabilities(&f.modem.platform, &capabilities) == H2_PAL_OK);
    assert(!(capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER));
    assert(h2_pal_modem_set_power_policy(&f.modem.platform, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
           H2_PAL_ERR_UNSUPPORTED);
    h2_quectel_modem_deinit(&f.modem);
    config.sleep_gate = gate;
    config.sync_api = NULL;
    assert(h2_quectel_modem_init(&f.modem, &config) == H2_PAL_OK);
    assert(h2_pal_modem_set_power_policy(&f.modem.platform, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
           H2_PAL_ERR_UNSUPPORTED);
    finish(&f);
    init_fixture(&f, &events, 0);
    f.model = "EC200U\r\nOK\r\n";
    assert(h2_pal_modem_open(&f.modem.platform, 0u) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_modem_set_power_policy(&f.modem.platform, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
           H2_PAL_ERR_UNSUPPORTED);
    finish(&f);
    init_fixture(&f, &events, 1);
    f.sim_level = 1;
    assert(h2_pal_modem_open(&f.modem.platform, 0u) == H2_PAL_ERR_INVALID_STATE);
    assert(f.modem.sim_restart_required);
    f.sim_level = 0;
    assert(h2_pal_modem_open(&f.modem.platform, 0u) == H2_PAL_ERR_INVALID_STATE);
    finish(&f);
}

static void *locate_thread(void *user) {
    fixture_t *f = user;
    h2_pal_modem_cell_location_t location;
    assert(h2_pal_modem_cell_locate(&f->modem.platform, 0u, &location) == H2_PAL_OK);
    assert(location.valid);
    return NULL;
}
static void *close_thread(void *user) {
    fixture_t *f = user;
    assert(h2_pal_modem_close(&f->modem.platform, 0u) == H2_PAL_OK);
    return NULL;
}
static void test_concurrent_close(void) {
    fixture_t f;
    h2_pal_system_event_api_t events;
    init_fixture(&f, &events, 0);
    assert(h2_pal_modem_open(&f.modem.platform, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_set_power_policy(&f.modem.platform, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) ==
           H2_PAL_OK);
    f.block_command = 1;
    pthread_t locate, close;
    assert(pthread_create(&locate, NULL, locate_thread, &f) == 0);
    assert(pthread_mutex_lock(&f.barrier) == 0);
    while (!f.command_entered) {
        assert(pthread_cond_wait(&f.condition, &f.barrier) == 0);
    }
    assert(!f.asleep_allowed && f.deinit_count == 0u);
    assert(pthread_create(&close, NULL, close_thread, &f) == 0);
    f.release_command = 1;
    assert(pthread_cond_broadcast(&f.condition) == 0);
    assert(pthread_mutex_unlock(&f.barrier) == 0);
    assert(pthread_join(locate, NULL) == 0);
    assert(pthread_join(close, NULL) == 0);
    assert(f.deinit_count == 1u && !f.modem.opened);
    finish(&f);
}

int main(void) {
    test_policy_and_holds();
    test_close_preserves_failed_sessions();
    test_sim_and_reset();
    test_unsupported_and_restart();
    test_concurrent_close();
    return 0;
}
