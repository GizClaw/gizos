#include "h2_modem_smoke.h"

#include <assert.h>
#include <string.h>

typedef struct test_state {
    size_t log_count;
    int saw_sim_unavailable;
    int saw_modem_unavailable;
    int saw_icmp;
    h2_pal_result_t open_result;
    int sim_ready;
    int opened;
    int closed;
    int status_calls;
    int apn_calls;
    int data_opened;
    int data_status_calls;
    int data_closed;
    int ping_calls;
} test_state_t;

static int log_write(void *user, h2_pal_log_level_t level, const char *scope, const char *message) {
    (void)level;
    test_state_t *state = (test_state_t *)user;
    assert(strcmp(scope, "modem-smoke") == 0);
    state->log_count++;
    if (strstr(message, "reason=sim_unavailable") != NULL) {
        state->saw_sim_unavailable = 1;
    }
    if (strstr(message, "reason=modem_unavailable") != NULL) {
        state->saw_modem_unavailable = 1;
    }
    if (strstr(message, "stage=icmp target=1.1.1.1") != NULL) {
        state->saw_icmp = 1;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t modem_open(void *user, uint32_t timeout_ms) {
    (void)timeout_ms;
    test_state_t *state = (test_state_t *)user;
    state->opened++;
    return state->open_result;
}

static h2_pal_result_t modem_close(void *user, uint32_t timeout_ms) {
    (void)timeout_ms;
    ((test_state_t *)user)->closed++;
    return H2_PAL_OK;
}

static h2_pal_result_t modem_identity(void *user, h2_pal_modem_identity_t *out_identity) {
    (void)user;
    memset(out_identity, 0, sizeof(*out_identity));
    strcpy(out_identity->manufacturer, "SIMCOM INCORPORATED");
    strcpy(out_identity->model, "A7670E-FASE");
    strcpy(out_identity->imei, "123456789012345");
    return H2_PAL_OK;
}

static h2_pal_result_t modem_status(void *user, h2_pal_modem_status_t *out_status) {
    test_state_t *state = (test_state_t *)user;
    memset(out_status, 0, sizeof(*out_status));
    state->status_calls++;
    out_status->sim = state->sim_ready
        ? H2_PAL_MODEM_SIM_STATE_READY
        : H2_PAL_MODEM_SIM_STATE_ABSENT;
    out_status->registration = state->sim_ready && state->status_calls > 1
        ? H2_PAL_MODEM_REGISTRATION_HOME
        : H2_PAL_MODEM_REGISTRATION_SEARCHING;
    return H2_PAL_OK;
}

static h2_pal_result_t modem_set_apn(
    void *user,
    const h2_pal_modem_apn_config_t *config) {
    test_state_t *state = (test_state_t *)user;
    assert(strcmp(config->apn, "internet") == 0);
    state->apn_calls++;
    return H2_PAL_OK;
}

static h2_pal_result_t modem_data_open(void *user, uint32_t timeout_ms) {
    test_state_t *state = (test_state_t *)user;
    assert(timeout_ms == 90000u);
    state->data_opened++;
    return H2_PAL_OK;
}

static h2_pal_result_t modem_data_close(void *user, uint32_t timeout_ms) {
    test_state_t *state = (test_state_t *)user;
    assert(timeout_ms == 10000u);
    state->data_closed++;
    return H2_PAL_OK;
}

static h2_pal_result_t modem_data_status(
    void *user,
    h2_pal_modem_data_status_t *out_status) {
    test_state_t *state = (test_state_t *)user;
    state->data_status_calls++;
    *out_status = (h2_pal_modem_data_status_t){
        .state = H2_PAL_MODEM_DATA_OPEN,
        .ip4 = 0x01020304u,
        .ip4_valid = 1u,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t net_icmp_echo(
    void *user,
    const h2_pal_net_addr_t *addr,
    const h2_pal_net_bind_t *bind,
    uint32_t timeout_ms,
    h2_pal_net_icmp_echo_result_t *out_result) {
    test_state_t *state = (test_state_t *)user;
    assert(addr->family == H2_PAL_NET_FAMILY_IPV4);
    assert(memcmp(addr->ip, (uint8_t[]){ 1u, 1u, 1u, 1u }, 4u) == 0);
    assert(bind == NULL && timeout_ms == 10000u);
    state->ping_calls++;
    *out_result = (h2_pal_net_icmp_echo_result_t){
        .elapsed_ms = 12u,
        .transmitted = 1u,
        .received = 1u,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t time_monotonic(void *user, uint64_t *out_ms) {
    (void)user;
    *out_ms = 100u;
    return H2_PAL_OK;
}

int main(void) {
    test_state_t state = { 0 };
    const h2_pal_log_vtable_t log_vtable = { .write = log_write };
    const h2_pal_log_api_t log = { .user = &state, .vtable = &log_vtable };
    const h2_pal_modem_vtable_t modem_vtable = {
        .open = modem_open,
        .close = modem_close,
        .get_identity = modem_identity,
        .get_status = modem_status,
        .set_apn = modem_set_apn,
        .data_open = modem_data_open,
        .data_close = modem_data_close,
        .get_data_status = modem_data_status,
    };
    h2_pal_modem_t modem = { .user = &state, .vtable = &modem_vtable };
    const h2_pal_net_vtable_t net_vtable = { .icmp_echo = net_icmp_echo };
    const h2_pal_net_api_t net = { .user = &state, .vtable = &net_vtable };
    const h2_pal_time_vtable_t time_vtable = { .get_monotonic_ms = time_monotonic };
    const h2_pal_time_api_t time = { .user = &state, .vtable = &time_vtable };
    h2_runtime_t runtime = {
        .log = &log,
        .modem = &modem,
        .net = &net,
        .time = &time,
    };
    assert(h2_modem_smoke_run(&runtime, NULL) == H2_PAL_OK);
    assert(state.opened == 1 && state.closed == 1);
    assert(state.log_count >= 6u && state.saw_sim_unavailable != 0);

    state = (test_state_t){ .sim_ready = 1 };
    const h2_modem_smoke_config_t config = { .apn = "internet" };
    assert(h2_modem_smoke_run(&runtime, &config) == H2_PAL_OK);
    assert(state.opened == 1 && state.closed == 1);
    assert(state.status_calls == 2 && state.apn_calls == 1);
    assert(state.data_opened == 1 && state.data_status_calls == 1 && state.data_closed == 1);
    assert(state.ping_calls == 1 && state.saw_icmp != 0);
    assert(state.saw_sim_unavailable == 0);

    state = (test_state_t){ .open_result = H2_PAL_ERR_IO };
    assert(h2_modem_smoke_run(&runtime, NULL) == H2_PAL_OK);
    assert(state.opened == 1 && state.closed == 1);
    assert(state.data_opened == 0 && state.ping_calls == 0);
    assert(state.saw_modem_unavailable != 0);
    return 0;
}
