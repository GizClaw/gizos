#ifndef H2_PAL_APPLICATION_HOST_H
#define H2_PAL_APPLICATION_HOST_H

#include "h2/pal/core/h2_pal_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque composition arguments; the base PAL target does not depend on Runtime.
 */
struct h2_runtime;
struct h2_runtime_config;

typedef enum h2_pal_application_host_event {
    H2_PAL_APPLICATION_HOST_RUNNING,
    H2_PAL_APPLICATION_HOST_READY,
    H2_PAL_APPLICATION_HOST_STOPPING,
} h2_pal_application_host_event_t;

/** Platform half of Application Host. All slots are required.
 * prepare/poll/release run on the calling thread (Desktop main, Web C main
 * Worker, or an SDK/BSP-owned task). prepare fills a complete Runtime config
 * and may acquire resources. release must accept partial prepare, including
 * prepare failure. It is called only after every Runtime user has joined.
 * A failed release leaves all still-live resources and its context retained.
 *
 * activate/deactivate run on the Host control task. activate installs external
 * producers after Runtime creation; deactivate closes admission and drains
 * callbacks even after partial activate failure, before Runtime destruction.
 * An error from deactivate retains the entire graph without calling release.
 * poll imports native events and sets out_stop for cooperative shutdown; it
 * must return promptly so the calling thread can continue servicing its UI.
 * notify may run on the App or calling thread and must be thread-safe.
 *
 * API descriptors are copied by the composition. user, vtable, configuration
 * pointers and callback contexts remain borrowed until successful cleanup;
 * on retention their owner must keep them alive indefinitely. No callback may
 * free the provider, Runtime or Host. Platform policy never selects an App,
 * private board, component mapping, required components or H2Loader behavior.
 */
typedef struct h2_pal_application_host_vtable {
    h2_pal_result_t (*prepare)(void *user,
                               struct h2_runtime_config *out_config);
    h2_pal_result_t (*activate)(void *user, struct h2_runtime *runtime);
    h2_pal_result_t (*poll)(void *user, int *out_stop);
    h2_pal_result_t (*deactivate)(void *user);
    h2_pal_result_t (*release)(void *user);
    void (*notify)(void *user, h2_pal_application_host_event_t event);
} h2_pal_application_host_vtable_t;

typedef struct h2_pal_application_host_api {
    void *user;
    const h2_pal_application_host_vtable_t *vtable;
} h2_pal_application_host_api_t;

#ifdef __cplusplus
}
#endif
#endif
