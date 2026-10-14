#ifndef H2_PAL_E2E_APPLICATION_HOST_H
#define H2_PAL_E2E_APPLICATION_HOST_H
#include "h2_application_host.h"
#include "h2_loader_boot.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Run the selected BSP through the shared Host. Uses the ordinary Host config;
 * provider must be NULL, allocator defaults to the native allocator. name and
 * capabilities select serial recovery identity; NULL name attaches no service.
 * All user hooks retain their Host ordering and receive the original user
 * context. E2E owns serial H2Loader shutdown before Runtime destruction,
 * including partial startup failures. Retention keeps this owner task/context
 * alive indefinitely. Device suites keep their own fixtures, watchdogs,
 * confirmation and replay. */
h2_pal_result_t
h2_pal_e2e_application_host_run(const char *name, uint32_t capabilities,
                                const h2_application_host_config_t *config,
                                h2_application_host_result_t *result);
/** Resident terminal reporting may wait here until cooperative Host Stop. */
h2_pal_result_t h2_pal_e2e_application_host_wait(h2_application_host_t *host,
                                                 h2_runtime_t *runtime);
/** A failed suite may have retained fixture workers/handles. Preserve its
 * Runtime graph on terminal Stop; successful qualification can cleanly exit. */
h2_pal_result_t h2_pal_e2e_application_host_finish(h2_application_host_t *host,
                                                   h2_runtime_t *runtime,
                                                   h2_pal_result_t result);
#ifdef __cplusplus
}
#endif
#endif
