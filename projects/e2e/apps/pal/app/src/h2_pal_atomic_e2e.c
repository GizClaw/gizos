#include "h2_pal_atomic_e2e.h"

void h2_pal_atomic_e2e_direct_c11_worker(void *argument) {
    h2_pal_atomic_e2e_c11_control_t *control = argument;
    for (uint32_t i = 0; i < control->iterations; ++i) {
        atomic_fetch_add_explicit(control->counter, 1, memory_order_relaxed);
        if (control->yield != NULL && (i & 4095u) == 4095u) control->yield(control->user);
    }
}

typedef struct h2_pal_atomic_e2e_state {
    const h2_pal_atomic_e2e_config_t *config;
    h2_pal_atomic_e2e_case_t test_case;
    uint32_t iterations;
    uint32_t guarded_count;
} h2_pal_atomic_e2e_state_t;

static void h2_pal_atomic_e2e_worker_run(void *argument) {
    h2_pal_atomic_e2e_state_t *state = argument;
    const h2_pal_atomic_e2e_config_t *config = state->config;
    for (uint32_t i = 0; i < state->iterations; ++i) {
        uint32_t previous;
        switch (state->test_case) {
        case H2_PAL_ATOMIC_E2E_FETCH_ADD:
            atomic_fetch_add_explicit(config->counter, 1, memory_order_relaxed);
            break;
        case H2_PAL_ATOMIC_E2E_I32_FETCH_ADD:
            atomic_fetch_add_explicit(config->signed_counter, 1, memory_order_relaxed);
            break;
        case H2_PAL_ATOMIC_E2E_CAS:
            previous = atomic_load_explicit(config->counter, memory_order_relaxed);
            while (!atomic_compare_exchange_weak_explicit(config->counter, &previous,
                       previous + 1, memory_order_acq_rel, memory_order_relaxed)) {}
            break;
        case H2_PAL_ATOMIC_E2E_EXCHANGE:
            while (atomic_exchange_explicit(config->counter, 1, memory_order_acquire) != 0) {}
            ++state->guarded_count;
            atomic_store_explicit(config->counter, 0, memory_order_release);
            break;
        case H2_PAL_ATOMIC_E2E_FLAG:
            while (atomic_flag_test_and_set_explicit(config->flag, memory_order_acquire)) {}
            ++state->guarded_count;
            atomic_flag_clear_explicit(config->flag, memory_order_release);
            break;
        }
        if (config->yield != NULL && (i & 4095u) == 4095u) config->yield(config->user);
    }
}

h2_pal_result_t h2_pal_atomic_e2e_run_case(
    const h2_pal_atomic_e2e_config_t *config, h2_pal_atomic_e2e_case_t test_case,
    uint32_t iterations, uint32_t *out_actual) {
    if (out_actual != NULL) *out_actual = 0;
    if (config == NULL || config->counter == NULL || config->signed_counter == NULL ||
        config->flag == NULL || config->run_pair == NULL || out_actual == NULL ||
        iterations == 0 || iterations > UINT32_MAX / 2 ||
        (unsigned)test_case > (unsigned)H2_PAL_ATOMIC_E2E_FLAG) return H2_PAL_ERR_INVALID_ARG;
    atomic_store(config->counter, 0);
    atomic_store(config->signed_counter, 0);
    atomic_flag_clear(config->flag);
    h2_pal_atomic_e2e_state_t state = {
        .config = config, .test_case = test_case, .iterations = iterations,
    };
    h2_pal_result_t rc = config->run_pair(config->user, h2_pal_atomic_e2e_worker_run,
                                             &state, &state);
    if (rc != H2_PAL_OK) return rc;
    if (test_case == H2_PAL_ATOMIC_E2E_EXCHANGE || test_case == H2_PAL_ATOMIC_E2E_FLAG)
        *out_actual = state.guarded_count;
    else if (test_case == H2_PAL_ATOMIC_E2E_I32_FETCH_ADD)
        *out_actual = (uint32_t)atomic_load(config->signed_counter);
    else
        *out_actual = atomic_load(config->counter);
    return *out_actual == iterations * 2 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
