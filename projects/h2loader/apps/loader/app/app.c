#include "h2loader_app.h"

#include <string.h>

typedef struct h2loader_serve_context {
    const h2loader_app_config_t *config;
    h2_loader_t *loader;
    h2_loader_command_t *command;
    h2_loader_startup_action_t action;
    int result;
} h2loader_serve_context_t;

static void serve_task(void *ctx) {
    h2loader_serve_context_t *serve = (h2loader_serve_context_t *)ctx;
    serve->result = serve->config->serve(
        serve->config->user,
        serve->loader,
        serve->command,
        serve->action);
}

static void destroy_operation_mutexes(
    h2_runtime_t *runtime,
    h2_pal_mutex_t *operation_mutex,
    h2_pal_mutex_t *wifi_operation_mutex) {
    if (wifi_operation_mutex != NULL) {
        (void)h2_pal_mutex_destroy(runtime->sync, wifi_operation_mutex);
    }
    if (operation_mutex != NULL) {
        (void)h2_pal_mutex_destroy(runtime->sync, operation_mutex);
    }
}

static void start_additional_command_service(
    h2_runtime_t *runtime,
    const h2loader_app_command_service_api_t *command_service,
    const h2_loader_command_config_t *command,
    const char *board,
    uint32_t capabilities,
    void **out_service) {
    if (command_service == NULL) {
        return;
    }
    int rc = command_service->open(
        command_service->user,
        runtime,
        command,
        board,
        capabilities,
        out_service);
    if (rc != H2_PAL_OK) {
        /* Keep the independent serial command task available for recovery
         * after an additional command service fails to start. */
        *out_service = NULL;
    }
}

static int close_additional_command_service(
    const h2loader_app_command_service_api_t *command_service,
    void *service) {
    if (service == NULL) {
        return H2_PAL_OK;
    }
    return command_service->close(command_service->user, service);
}

int h2loader_app_run_with_command_service(
    h2_runtime_t *runtime,
    const h2loader_app_config_t *config,
    const h2loader_app_command_service_api_t *command_service) {
    h2_loader_startup_action_t action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
    h2_loader_command_config_t command;
    h2_loader_command_t command_state;
    h2_loader_config_t loader_config;
    h2_loader_t loader;
    h2loader_serve_context_t serve_context;
    h2_pal_task_t *serve_handle = NULL;
    h2_pal_mutex_t *operation_mutex = NULL;
    h2_pal_mutex_t *wifi_operation_mutex = NULL;
    void *additional_command_service = NULL;
    int serve_stop_rc = H2_PAL_OK;
    int prepare_rc = H2_PAL_OK;
    int rc;

    if (runtime == NULL || config == NULL || config->serve == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->before_startup != NULL && config->stop_serve == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (command_service != NULL &&
        (command_service->open == NULL || command_service->close == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    loader_config = config->loader;
    if (loader_config.package.allocator == NULL) {
        loader_config.package.allocator = runtime->mem;
    }
    if (loader_config.pref == NULL) {
        loader_config.pref = runtime->pref;
    }
    if (loader_config.power == NULL) {
        loader_config.power = runtime->power;
    }
    if (loader_config.board == NULL) {
        loader_config.board = runtime->board;
    }
    if (loader_config.target == NULL) {
        loader_config.target = runtime->target;
    }
    if (loader_config.chip == NULL) {
        loader_config.chip = runtime->chip;
    }

    memset(&loader, 0, sizeof(loader));
    rc = h2_loader_init(&loader, &loader_config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (config->prepare != NULL) {
        prepare_rc = config->prepare(config->user, &loader);
        loader.force_command_mode = prepare_rc != H2_PAL_OK;
    }

    command = config->command;
    command.loader = &loader;
    if (command.fs == NULL) {
        command.fs = runtime->fs;
    }
    if (command.http == NULL) {
        command.http = runtime->http;
    }
    if (command.wifi == NULL) {
        command.wifi = runtime->wifi_sta;
    }
    if (command.wifi_settings == NULL) {
        command.wifi_settings = runtime->wifi_settings;
    }
    if (command.disk == NULL) {
        command.disk = runtime->disk;
    }
    const h2_pal_mutex_config_t mutex_config = {
        .name = "h2loader-operation",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
    };
    rc = h2_pal_mutex_create(runtime->sync, &mutex_config, &operation_mutex);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const h2_pal_mutex_config_t wifi_mutex_config = {
        .name = "h2loader-wifi",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
    };
    rc = h2_pal_mutex_create(
        runtime->sync, &wifi_mutex_config, &wifi_operation_mutex);
    if (rc != H2_PAL_OK) {
        destroy_operation_mutexes(runtime, operation_mutex, NULL);
        return rc;
    }
    command.operation_sync = runtime->sync;
    command.operation_mutex = operation_mutex;
    command.wifi_operation_sync = runtime->sync;
    command.wifi_operation_mutex = wifi_operation_mutex;
    command.defer_app_install = config->before_startup != NULL;
    rc = h2_loader_command_init(&command_state, &command);
    if (rc != H2_PAL_OK) {
        destroy_operation_mutexes(
            runtime, operation_mutex, wifi_operation_mutex);
        return rc;
    }
    if (config->before_startup != NULL) {
        const h2_pal_task_options_t options = {
            .name = "h2loader/appcmd",
            .min_stack_size = 49152u,
        };
        loader.force_command_mode = 1;
        rc = h2_loader_startup(&loader, &action);
        if (rc != H2_PAL_OK) {
            (void)close_additional_command_service(
                command_service, additional_command_service);
            destroy_operation_mutexes(
                runtime, operation_mutex, wifi_operation_mutex);
            return rc;
        }
        if (action != H2_LOADER_STARTUP_ACTION_COMMAND_MODE) {
            (void)close_additional_command_service(
                command_service, additional_command_service);
            destroy_operation_mutexes(
                runtime, operation_mutex, wifi_operation_mutex);
            return H2_PAL_OK;
        }
        start_additional_command_service(
            runtime,
            command_service,
            &command,
            loader_config.board,
            loader_config.hardware_capabilities,
            &additional_command_service);
        serve_context = (h2loader_serve_context_t){
            .config = config,
            .loader = &loader,
            .command = &command_state,
            .action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE,
            .result = H2_PAL_OK,
        };
        rc = h2_pal_task_start(
            runtime->task,
            &options,
            serve_task,
            &serve_context,
            &serve_handle);
        if (rc != H2_PAL_OK) {
            (void)close_additional_command_service(
                command_service, additional_command_service);
            destroy_operation_mutexes(
                runtime, operation_mutex, wifi_operation_mutex);
            return rc;
        }
        for (;;) {
            int before_startup_rc = config->before_startup(
                config->before_startup_user, &loader, runtime->sync,
                operation_mutex, runtime->sync, wifi_operation_mutex);
            if (before_startup_rc != H2_PAL_OK &&
                before_startup_rc != H2_PAL_EXIT) {
                (void)h2_loader_set_last_result(
                    &loader, before_startup_rc);
                loader.status.last_result = before_startup_rc;
                action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
                break;
            }
            loader.force_command_mode = prepare_rc != H2_PAL_OK;
            rc = h2_pal_mutex_lock(runtime->sync, operation_mutex);
            if (rc == H2_PAL_OK) {
                rc = h2_loader_startup(&loader, &action);
                int unlock_rc = h2_pal_mutex_unlock(
                    runtime->sync, operation_mutex);
                if (rc == H2_PAL_OK) {
                    rc = unlock_rc;
                }
            }
            if (rc != H2_PAL_OK) {
                (void)h2_loader_set_last_result(&loader, rc);
                loader.status.last_result = rc;
                action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
            }
            if (before_startup_rc == H2_PAL_EXIT && rc == H2_PAL_OK &&
                action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE &&
                config->rearm_before_startup != NULL &&
                loader.status.install_state !=
                    H2_LOADER_INSTALL_STATE_INSTALL_FAILED &&
                loader.status.install_state !=
                    H2_LOADER_INSTALL_STATE_MAIN_FAILED) {
                config->rearm_before_startup(config->before_startup_user);
                continue;
            }
            break;
        }
    } else {
        rc = h2_loader_startup(&loader, &action);
    }
    if (rc != H2_PAL_OK) {
        (void)h2_loader_set_last_result(&loader, rc);
        loader.status.last_result = rc;
        action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
    } else if (prepare_rc != H2_PAL_OK) {
        (void)h2_loader_set_last_result(&loader, prepare_rc);
        loader.status.last_result = prepare_rc;
    }
    if (config->before_startup == NULL &&
        action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE) {
        start_additional_command_service(
            runtime,
            command_service,
            &command,
            loader_config.board,
            loader_config.hardware_capabilities,
            &additional_command_service);
    }
    if (serve_handle == NULL) {
        const h2_pal_task_options_t options = {
            .name = "h2loader/appcmd",
            .min_stack_size = 49152u,
        };
        serve_context = (h2loader_serve_context_t){
            .config = config,
            .loader = &loader,
            .command = &command_state,
            .action = action,
            .result = H2_PAL_OK,
        };
        rc = h2_pal_task_start(
            runtime->task,
            &options,
            serve_task,
            &serve_context,
            &serve_handle);
        if (rc != H2_PAL_OK) {
            (void)close_additional_command_service(
                command_service, additional_command_service);
            destroy_operation_mutexes(
                runtime, operation_mutex, wifi_operation_mutex);
            return rc;
        }
    }
    if (serve_handle != NULL &&
        action != H2_LOADER_STARTUP_ACTION_COMMAND_MODE) {
        serve_stop_rc = config->stop_serve != NULL
            ? config->stop_serve(config->user)
            : H2_PAL_OK;
    }
    if (serve_handle != NULL) {
        rc = h2_pal_task_join(runtime->task, serve_handle);
        int result = serve_stop_rc != H2_PAL_OK
            ? serve_stop_rc
            : (rc == H2_PAL_OK ? serve_context.result : rc);
        int close_rc = close_additional_command_service(
            command_service, additional_command_service);
        if (result == H2_PAL_OK) {
            result = close_rc;
        }
        destroy_operation_mutexes(
            runtime, operation_mutex, wifi_operation_mutex);
        return result;
    }
    (void)close_additional_command_service(
        command_service, additional_command_service);
    destroy_operation_mutexes(runtime, operation_mutex, wifi_operation_mutex);
    return H2_PAL_ERR_INVALID_STATE;
}

int h2loader_app_run(
    h2_runtime_t *runtime,
    const h2loader_app_config_t *config) {
    return h2loader_app_run_with_command_service(runtime, config, NULL);
}
