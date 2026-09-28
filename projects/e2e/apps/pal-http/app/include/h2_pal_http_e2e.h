#ifndef H2_PAL_HTTP_E2E_H
#define H2_PAL_HTTP_E2E_H

#include "h2_runtime.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_http_e2e_case {
#define H2_PAL_HTTP_CASE(symbol, id) H2_PAL_HTTP_E2E_##symbol,
#include "h2_pal_http_cases.inc"
#undef H2_PAL_HTTP_CASE
    H2_PAL_HTTP_E2E_CASE_COUNT
} h2_pal_http_e2e_case_t;

typedef struct h2_pal_http_e2e_case_result {
    const char *id;
    int passed;
    int blocked;
    int detail;
    unsigned line;
} h2_pal_http_e2e_case_result_t;

typedef struct h2_pal_http_e2e_result {
    unsigned passed;
    unsigned failed;
    unsigned blocked;
    size_t retained_allocations;
    h2_pal_http_e2e_case_result_t cases[H2_PAL_HTTP_E2E_CASE_COUNT];
} h2_pal_http_e2e_result_t;

typedef struct h2_pal_http_e2e_config {
    /** Borrowed Runtime with real HTTP, Memory and monotonic Time providers. */
    const h2_runtime_t *runtime;
    /** Borrowed NUL-terminated fixture bases, including a unique session path. */
    const char *http_base;
    const char *https_base;
    const char *untrusted_https_base;
    /** Optional progress callback; invoked synchronously after case cleanup. */
    void (*report)(void *user, const h2_pal_http_e2e_case_result_t *result);
    void *report_user;
} h2_pal_http_e2e_config_t;

/**
 * @brief Run every HTTP contract case synchronously against an owned fixture.
 *
 * The App borrows all dependencies, closes every response and retains no
 * callback or buffer after return. No network, filesystem, environment or
 * platform lifecycle is assembled here. Missing callbacks block qualification.
 * Returns OK only when every mandatory case passes with no retained allocation.
 */
int h2_pal_http_e2e_run(const h2_pal_http_e2e_config_t *config,
                      h2_pal_http_e2e_result_t *out_result);

#ifdef __cplusplus
}
#endif
#endif
