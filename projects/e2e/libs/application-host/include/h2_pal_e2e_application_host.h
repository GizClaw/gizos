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
 * alive indefinitely. An App entry error automatically retains its Runtime
 * graph on Stop, since failed device fixtures may still borrow it. Serial
 * recovery remains live while waiting for Stop after an App error. Device
 * suites keep their own fixtures, watchdogs, confirmation and replay. */
h2_pal_result_t
h2_pal_e2e_application_host_run(const char *name, uint32_t capabilities,
                                const h2_application_host_config_t *config,
                                h2_application_host_result_t *result);
/** Install the target task policy and start the native SDK/BSP owner, then run
 * the shared Host. Call once from firmware main/app_main. BK initializes its
 * SDK; both SDKs start the BSP entry task as the Host owner.
 * Copies name and config for asynchronous startup; config.user and callback
 * dependencies remain borrowed until Host completion or throughout retention.
 * Requires nonempty name and the same config/capabilities as run(). Returns
 * startup errors (ESP policy failure parks its SDK owner); asynchronous Host
 * errors are logged and keep the BSP owner alive.
 */
h2_pal_result_t h2_pal_e2e_application_host_start(
    const char *name, uint32_t capabilities,
    const h2_application_host_config_t *config);
/** Keep the native owner alive indefinitely without releasing borrowed SDK or
 * fixture state. Also used by diagnostic workers after their deadline. */
void h2_pal_e2e_application_host_park(void);
/** Report a fatal setup stage and park its owner. Does not confirm an image,
 * request Stop, destroy Runtime, or release dependencies. */
void h2_pal_e2e_application_host_fail(const char *stage, h2_pal_result_t result);
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
