#ifndef H2LOADER_APP_H
#define H2LOADER_APP_H

#include "h2_loader_command.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Command-service adapter selected by a concrete firmware entry.
 *
 * The API and its user context are borrowed until h2loader_app_run_with_command_service()
 * returns. open() borrows all input arguments, stores an owned service handle in
 * out_service on success, and leaves out_service NULL on failure. close() releases
 * that handle.
 */
typedef struct h2loader_app_command_service_api {
    void *user;
    int (*open)(
        void *user,
        h2_runtime_t *runtime,
        const h2_loader_command_config_t *command,
        const char *board,
        uint32_t capabilities,
        void **out_service);
    int (*close)(void *user, void *service);
} h2loader_app_command_service_api_t;

typedef struct h2loader_app_config {
    h2_loader_config_t loader;
    h2_loader_command_config_t command;
    void *user;
    int (*prepare)(void *user, h2_loader_t *loader);
    void *before_startup_user;
    /**
     * Optional blocking board gate run concurrently with serve(). The borrowed
     * mutexes serialize Loader operations and Wi-Fi ownership respectively.
     */
    int (*before_startup)(void *user, h2_loader_t *loader,
                          const h2_pal_sync_api_t *operation_sync,
                          h2_pal_mutex_t *operation_mutex,
                          const h2_pal_sync_api_t *wifi_operation_sync,
                          h2_pal_mutex_t *wifi_operation_mutex);
    void (*rearm_before_startup)(void *user);
    /** Requests the concurrently running serve() callback to return. */
    int (*stop_serve)(void *user);
    int (*serve)(
        void *user,
        h2_loader_t *loader,
        h2_loader_command_t *command,
        h2_loader_startup_action_t action);
} h2loader_app_config_t;

/**
 * Runs the blocking portable Loader app.
 *
 * runtime, config and all callback contexts are borrowed until the call
 * returns. serve always runs on the target's h2loader/appcmd task and is
 * stopped when required and joined before this function returns.
 */
int h2loader_app_run(
    h2_runtime_t *runtime,
    const h2loader_app_config_t *config);

/**
 * Runs the blocking portable Loader app with an entry-selected command service.
 *
 * command_service is borrowed until this function returns. The serial command
 * service remains available if the additional service cannot be opened.
 */
int h2loader_app_run_with_command_service(
    h2_runtime_t *runtime,
    const h2loader_app_config_t *config,
    const h2loader_app_command_service_api_t *command_service);

#ifdef __cplusplus
}
#endif

#endif
