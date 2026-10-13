#ifndef H2_APPLICATION_HOST_H
#define H2_APPLICATION_HOST_H

#include "h2/pal/application/h2_pal_application_host.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_application_host h2_application_host_t;
typedef h2_pal_result_t (*h2_application_host_entry_fn)(
    h2_application_host_t *host, h2_runtime_t *runtime, void *user);

/** All descriptors are copied. Opaque contexts and strings remain borrowed
 * through successful run/destroy, indefinitely on retention. Use persistent
 * storage for contexts that can outlive run. allocator must outlive destroy
 * and must be independent of provider resources released by run.
 * configure/release_runtime pair owns product resources; release_runtime also
 * runs after configure failure. validate checks required components before
 * external input starts. App entry must stop/join its own workers before
 * returning, or call retain when it cannot prove their exit. No forced task
 * cancellation can prove dependent workers safe. All hooks are optional.
 */
typedef struct h2_application_host_config {
    const h2_pal_mem_api_t *allocator;
    const h2_pal_application_host_api_t *provider;
    h2_application_host_entry_fn entry;
    void *user;
    h2_pal_result_t (*configure)(void *user, h2_runtime_config_t *config);
    h2_pal_result_t (*validate)(void *user, h2_runtime_t *runtime);
    h2_pal_result_t (*release_runtime)(void *user);
    uint32_t run_ms;
    size_t stack_size;
    /** Control task minimum bytes; zero selects 64 KiB. */
    size_t control_stack_size;
} h2_application_host_config_t;

typedef struct h2_application_host_result {
    h2_pal_result_t application;
    h2_pal_result_t cleanup;
    int retained;
} h2_application_host_result_t;

/** Allocate an owned, single-use Host. On failure *out_host is NULL. */
h2_pal_result_t
h2_application_host_create(const h2_application_host_config_t *config,
                           h2_application_host_t **out_host);
/** Blocking shared prepare/run/stop/join/Runtime-destroy/platform-release
 * flow. Call from the platform's allowed startup context, never a callback.
 * Cooperative stop waits without a deadline for entry and its workers to
 * exit, while poll keeps the platform event loop responsive. Any uncertain
 * join/cleanup retains the graph; result.retained distinguishes that state
 * from an ordinary App error with complete cleanup. Single owner calls run.
 */
h2_pal_result_t
h2_application_host_run(h2_application_host_t *host,
                        h2_application_host_result_t *out_result);
/** Free the Host descriptor only after run cleanup or before run; BUSY keeps
 * *host intact while running or retained. Serial with run and all callbacks. */
h2_pal_result_t h2_application_host_destroy(h2_application_host_t **host);
/** Thread-safe cooperative stop, ready and retention from live callbacks.
 * Ready is accepted only while the App entry is active. */
void h2_application_host_request_stop(h2_application_host_t *host);
int h2_application_host_should_stop(void *host);
/** Thread-safe retention state for rejecting external admission. */
int h2_application_host_is_retained(const h2_application_host_t *host);
h2_pal_result_t h2_application_host_ready(void *host);
/** Latch an unsafe dependent cleanup; no ownership transfer or retry. */
h2_pal_result_t h2_application_host_retain(h2_application_host_t *host,
                                           void *owner, h2_pal_result_t reason);
/** Borrow the copied descriptor until successful destroy. */
const h2_pal_application_host_api_t *
h2_application_host_provider(const h2_application_host_t *host);

#ifdef __cplusplus
}
#endif
#endif
