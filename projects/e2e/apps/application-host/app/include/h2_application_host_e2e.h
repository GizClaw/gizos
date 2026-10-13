#ifndef H2_APPLICATION_HOST_E2E_H
#define H2_APPLICATION_HOST_E2E_H
#include "h2_application_host.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Portable test entry: report ready and honor the launcher's stop budget. */
h2_pal_result_t h2_application_host_e2e_run(h2_application_host_t *host,
                                            h2_runtime_t *runtime, void *user);
#ifdef __cplusplus
}
#endif
#endif
