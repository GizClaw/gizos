#ifndef H2_APPLICATION_HOST_SDK_H
#define H2_APPLICATION_HOST_SDK_H

#include "h2_application_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/** BSP/SDK injection used by ESP and BK providers. Callbacks and context are
 * borrowed through successful Host cleanup, indefinitely on retention.
 * startup and shutdown run on the SDK task which creates the provider;
 * startup fills a complete Runtime config. shutdown accepts partial startup.
 * acquire/release_input run on the Host control task and must close admission
 * and drain external producers before release_input returns. Optional hooks
 * are no-ops; should_stop and notify must tolerate calls from different tasks.
 * SDK task scheduling/stack policy is injected through the ordinary PAL Task.
 */
typedef struct h2_application_host_sdk_config {
    void *user;
    h2_pal_result_t (*startup)(void *user, h2_runtime_config_t *out_config);
    h2_pal_result_t (*shutdown)(void *user);
    h2_pal_result_t (*acquire_input)(void *user, h2_runtime_t *runtime);
    h2_pal_result_t (*release_input)(void *user);
    int (*should_stop)(void *user);
    void (*notify)(void *user, h2_pal_application_host_event_t event);
} h2_application_host_sdk_config_t;

/** Shared SDK hook dispatch; providers supply their native context checks and
 * scheduler yield. These do not own the App/Runtime lifecycle. */
h2_pal_result_t h2_application_host_sdk_activate(void *config,
                                                 h2_runtime_t *runtime);
h2_pal_result_t h2_application_host_sdk_deactivate(void *config);
void h2_application_host_sdk_notify(void *config,
                                    h2_pal_application_host_event_t event);

#ifdef __cplusplus
}
#endif
#endif
