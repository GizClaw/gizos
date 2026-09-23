#ifndef H2_PAL_ATOMIC_E2E_H
#define H2_PAL_ATOMIC_E2E_H

#include "h2/pal/os/h2_pal_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_atomic_e2e_case {
    H2_PAL_ATOMIC_E2E_FETCH_ADD,
    H2_PAL_ATOMIC_E2E_I32_FETCH_ADD,
    H2_PAL_ATOMIC_E2E_CAS,
    H2_PAL_ATOMIC_E2E_EXCHANGE,
    H2_PAL_ATOMIC_E2E_FLAG,
} h2_pal_atomic_e2e_case_t;

typedef h2_pal_result_t (*h2_pal_atomic_e2e_run_pair_fn_t)(
    void *user, void (*entry)(void *), void *first, void *second);

typedef void (*h2_pal_atomic_e2e_yield_fn_t)(void *user);

/** Borrowed dependencies and initialized storage, retained through run_case. */
typedef struct h2_pal_atomic_e2e_config {
    _Atomic uint32_t *counter;
    _Atomic int32_t *signed_counter;
    atomic_flag *flag;
    h2_pal_atomic_e2e_run_pair_fn_t run_pair;
    h2_pal_atomic_e2e_yield_fn_t yield;
    void *user;
} h2_pal_atomic_e2e_config_t;

/** Direct C11 control compiled with the same Bazel toolchain as GizOS. */
typedef struct h2_pal_atomic_e2e_c11_control {
    _Atomic uint32_t *counter;
    uint32_t iterations;
    h2_pal_atomic_e2e_yield_fn_t yield;
    void *user;
} h2_pal_atomic_e2e_c11_control_t;

void h2_pal_atomic_e2e_direct_c11_worker(void *argument);

/** Runs two concurrent workers supplied by the launcher and checks exact count. */
h2_pal_result_t h2_pal_atomic_e2e_run_case(
    const h2_pal_atomic_e2e_config_t *config, h2_pal_atomic_e2e_case_t test_case,
    uint32_t iterations, uint32_t *out_actual);

#ifdef __cplusplus
}
#endif

#endif
