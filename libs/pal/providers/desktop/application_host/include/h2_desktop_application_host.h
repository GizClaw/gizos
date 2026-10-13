#ifndef H2_DESKTOP_APPLICATION_HOST_H
#define H2_DESKTOP_APPLICATION_HOST_H

#include "h2_application_host.h"
#include "h2_sdl3.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_desktop_application_host h2_desktop_application_host_t;

/** Caller injects a complete Runtime config and owns its providers. This
 * provider owns only its SDL window; display/touch are replaced with that
 * window. Configuration pointers remain borrowed through successful destroy,
 * indefinitely if the shared Host retains the graph. Product mapping and
 * required-component validation belong to the shared Host hooks.
 */
typedef struct h2_desktop_application_host_config {
    h2_sdl3_config_t window;
    h2_runtime_config_t runtime;
    void *user;
    /** Bounded main-thread callback; event is borrowed only during this call.
     * Deactivate closes admission and drains callbacks before Runtime cleanup. */
    h2_pal_result_t (*event)(void *user, const h2_sdl3_event_t *event);
} h2_desktop_application_host_config_t;

/** Create on the process main thread, then inject api() into shared Host.
 * create/destroy and Host run must execute on that same main thread.
 */
h2_pal_result_t h2_desktop_application_host_create(
    const h2_desktop_application_host_config_t *config,
    h2_desktop_application_host_t **out_provider);
const h2_pal_application_host_api_t *
h2_desktop_application_host_api(h2_desktop_application_host_t *provider);
/** BUSY retains a provider whose window has not been released by Host. */
h2_pal_result_t
h2_desktop_application_host_destroy(h2_desktop_application_host_t **provider);

#ifdef __cplusplus
}
#endif
#endif
