#ifndef H2_PAL_JSON_E2E_H
#define H2_PAL_JSON_E2E_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_JSON_E2E_VERSION 1u

typedef enum h2_pal_json_e2e_status {
    H2_PAL_JSON_E2E_NOT_RUN = 0,
    H2_PAL_JSON_E2E_PASS,
    H2_PAL_JSON_E2E_FAIL,
    H2_PAL_JSON_E2E_BLOCKED,
} h2_pal_json_e2e_status_t;

enum {
    H2_PAL_JSON_E2E_CASE_COUNT = 0
#define H2_PAL_JSON_CASE(id, function) +1
#include "h2_pal_json_cases.inc"
#undef H2_PAL_JSON_CASE
};

typedef struct h2_pal_json_e2e_case {
    const char *id;
    h2_pal_json_e2e_status_t status;
    h2_pal_result_t result;
} h2_pal_json_e2e_case_t;

typedef struct h2_pal_json_e2e_result {
    h2_pal_json_e2e_case_t cases[H2_PAL_JSON_E2E_CASE_COUNT];
    size_t passed, failed, blocked, not_run;
    int complete, qualified;
} h2_pal_json_e2e_result_t;

typedef enum h2_pal_json_e2e_probe {
    H2_PAL_JSON_E2E_PROBE_LIVE_OUTPUT = 1,
    H2_PAL_JSON_E2E_PROBE_ALLOCATOR_FAILURE = 2,
} h2_pal_json_e2e_probe_t;

/** Launcher-owned lifecycle probe using the actual provider and Runtime Mem. */
typedef h2_pal_result_t (*h2_pal_json_e2e_probe_fn)(
    const h2_pal_mem_api_t *mem, h2_pal_json_e2e_probe_t probe);

/** Borrow an initialized Runtime and a separately owned JSON PAL provider.
 * The launcher owns provider creation/destruction and records teardown. */
h2_pal_result_t h2_pal_json_e2e_run(h2_runtime_t *runtime,
                                    const h2_pal_json_api_t *json,
                                    h2_pal_json_e2e_probe_fn probe,
                                    h2_pal_json_e2e_result_t *result);

#ifdef __cplusplus
}
#endif
#endif
