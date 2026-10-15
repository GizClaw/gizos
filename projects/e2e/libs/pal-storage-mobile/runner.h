#ifndef H2_STORAGE_MOBILE_RUNNER_H
#define H2_STORAGE_MOBILE_RUNNER_H
#include "h2_pal_storage_e2e.h"
/* Caller gives exclusive ownership of the configured Runtime and storage to
 * this phase. teardown runs after clean Runtime shutdown; failed close retains
 * ownership until the isolated App process is terminated by the driver. */
int h2_storage_mobile_phase(h2_runtime_config_t config, unsigned phase,
    uint32_t nonce, const char *report_path, int (*teardown)(void *), void *owner);
#endif
