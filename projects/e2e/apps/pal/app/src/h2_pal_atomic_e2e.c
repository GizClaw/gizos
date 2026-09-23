#include "h2_pal_atomic_e2e.h"

void h2_pal_atomic_e2e_direct_c11_worker(void *argument) {
    h2_pal_atomic_e2e_c11_control_t *control = argument;
    for (uint32_t i = 0; i < control->iterations; ++i) {
        atomic_fetch_add_explicit(control->counter, 1, memory_order_relaxed);
        if (control->yield != NULL && (i & 4095u) == 4095u) {
            control->yield(control->user);
        }
    }
}

typedef struct h2_pal_atomic_e2e_state {
    const h2_pal_atomic_e2e_config_t *config;
    h2_pal_atomic_e2e_case_t test_case;
    uint32_t iterations;
    uint32_t guarded_count;
} h2_pal_atomic_e2e_state_t;

typedef struct h2_pal_atomic_e2e_worker {
    h2_pal_atomic_e2e_state_t *state;
    h2_pal_result_t result;
} h2_pal_atomic_e2e_worker_t;

static void h2_pal_atomic_e2e_worker_run(void *argument) {
    h2_pal_atomic_e2e_worker_t *worker = argument;
    h2_pal_atomic_e2e_state_t *state = worker->state;
    const h2_pal_atomic_e2e_config_t *config = state->config;
    for (uint32_t i = 0; i < state->iterations; ++i) {
        uint32_t previous = 0;
        bool changed = false;
        bool flag_previous = false;
        h2_pal_result_t rc = H2_PAL_OK;
        switch (state->test_case) {
        case H2_PAL_ATOMIC_E2E_FETCH_ADD:
            rc = h2_pal_atomic_u32_fetch_add(config->atomic, config->counter, 1,
                &previous, H2_PAL_ATOMIC_RELAXED);
            break;
        case H2_PAL_ATOMIC_E2E_CAS:
            do {
                rc = h2_pal_atomic_u32_load(config->atomic, config->counter,
                    &previous, H2_PAL_ATOMIC_RELAXED);
                if (rc != H2_PAL_OK) break;
                rc = h2_pal_atomic_u32_compare_exchange(config->atomic,
                    config->counter, &previous, previous + 1, &changed,
                    H2_PAL_ATOMIC_ACQ_REL, H2_PAL_ATOMIC_RELAXED);
            } while (rc == H2_PAL_OK && !changed);
            break;
        case H2_PAL_ATOMIC_E2E_EXCHANGE:
            do {
                rc = h2_pal_atomic_u32_exchange(config->atomic, config->counter,
                    1, &previous, H2_PAL_ATOMIC_ACQUIRE);
            } while (rc == H2_PAL_OK && previous != 0);
            if (rc == H2_PAL_OK) {
                ++state->guarded_count;
                rc = h2_pal_atomic_u32_store(config->atomic, config->counter, 0,
                    H2_PAL_ATOMIC_RELEASE);
            }
            break;
        case H2_PAL_ATOMIC_E2E_FLAG:
            do {
                rc = h2_pal_atomic_flag_test_and_set(config->atomic, config->flag,
                    &flag_previous, H2_PAL_ATOMIC_ACQUIRE);
            } while (rc == H2_PAL_OK && flag_previous);
            if (rc == H2_PAL_OK) {
                ++state->guarded_count;
                rc = h2_pal_atomic_flag_clear(config->atomic, config->flag,
                    H2_PAL_ATOMIC_RELEASE);
            }
            break;
        }
        if (rc != H2_PAL_OK) {
            worker->result = rc;
            return;
        }
        if (config->yield != NULL && (i & 4095u) == 4095u) {
            config->yield(config->user);
        }
    }
    worker->result = H2_PAL_OK;
}

h2_pal_result_t h2_pal_atomic_e2e_run_case(
    const h2_pal_atomic_e2e_config_t *config, h2_pal_atomic_e2e_case_t test_case,
    uint32_t iterations, uint32_t *out_actual) {
    if (out_actual != NULL) *out_actual = 0;
    if (config == NULL || config->atomic == NULL || config->counter == NULL ||
        config->flag == NULL || config->run_pair == NULL || out_actual == NULL ||
        iterations == 0 || iterations > UINT32_MAX / 2 ||
        (unsigned)test_case > (unsigned)H2_PAL_ATOMIC_E2E_FLAG) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_pal_atomic_u32_store(config->atomic, config->counter,
        0, H2_PAL_ATOMIC_RELAXED);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_pal_atomic_flag_clear(config->atomic, config->flag,
        H2_PAL_ATOMIC_RELAXED);
    if (rc != H2_PAL_OK) return rc;
    h2_pal_atomic_e2e_state_t state = {
        .config = config, .test_case = test_case, .iterations = iterations,
    };
    h2_pal_atomic_e2e_worker_t workers[2] = {
        { .state = &state, .result = H2_PAL_ERR_INVALID_STATE },
        { .state = &state, .result = H2_PAL_ERR_INVALID_STATE },
    };
    rc = config->run_pair(config->user, h2_pal_atomic_e2e_worker_run,
        &workers[0], &workers[1]);
    if (rc != H2_PAL_OK) return rc;
    if (workers[0].result != H2_PAL_OK) return workers[0].result;
    if (workers[1].result != H2_PAL_OK) return workers[1].result;
    if (test_case == H2_PAL_ATOMIC_E2E_EXCHANGE ||
        test_case == H2_PAL_ATOMIC_E2E_FLAG) {
        *out_actual = state.guarded_count;
    } else {
        rc = h2_pal_atomic_u32_load(config->atomic, config->counter, out_actual,
            H2_PAL_ATOMIC_ACQUIRE);
        if (rc != H2_PAL_OK) return rc;
    }
    return *out_actual == iterations * 2 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
