#ifndef H2_HTTP_MOBILE_RUNNER_H
#define H2_HTTP_MOBILE_RUNNER_H
#include "h2_pal_http_e2e.h"
/* Borrow config/fixture inputs, destroy Runtime before returning to the owner. */
int h2_http_mobile_run(h2_runtime_config_t config, const char *http_base,
    const char *https_base, const char *untrusted_base, h2_pal_http_e2e_result_t *out);
/* Save a bounded report after the launcher retires HTTP and Core owners. */
int h2_http_mobile_report(const char *path, const char *platform, const char *version,
    const h2_pal_http_e2e_result_t *result, int rc, int teardown);
#endif
