/* App entry for one Lua script, compiled once per h2_lua_web_app().
 *
 * The generated h2_web_lua_app_config.h names the embedded script, the app
 * name and the optional exit Button; the web board (h2_web_application_board) supplies
 * the Buttons and display. PAL Application Host assembles the Web Runtime; this
 * entry runs one Lua job, passes every board Button's component id to the
 * script as `args.<name>`, forwards Button events to the job and ends it from
 * the exit Button or the host stop request. */
#include "h2_web_lua_app_config.h"

#include "h2_application_host_lua.h"
#include "h2_lua.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_web_application_host.h"

#if H2_WEB_LUA_APP_EXTENSION
#include "h2_web_lua_app.h"
#endif

#include <stdbool.h>
#include <stdio.h>

#include <string.h>

/* Component id of the exit Button (its board index + 1), or 0 for none. */
static h2_runtime_component_id_t exit_component(void) {
    for (size_t i = 0u; i < h2_web_application_board.button_count; ++i) {
        if (strcmp(h2_web_application_board.buttons[i].name, H2_WEB_LUA_APP_EXIT_BUTTON) ==
            0)
            return (h2_runtime_component_id_t)(i + 1u);
    }
    return H2_RUNTIME_COMPONENT_ID_NONE;
}

static bool exit_requested(const h2_runtime_event_t *event) {
#if H2_WEB_LUA_APP_EXTENSION
    if (h2_web_lua_app_extension.exit_requested != NULL)
        return h2_web_lua_app_extension.exit_requested(event);
#endif
    return event->kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
           event->payload_size >= sizeof(h2_runtime_button_action_event_t) &&
           h2_runtime_button_action_is_released(event->payload);
}

static h2_pal_result_t register_host(void *user, h2_lua_host_t *host) {
    (void)user;
#if H2_WEB_LUA_APP_EXTENSION
    if (h2_web_lua_app_extension.register_host != NULL)
        return h2_web_lua_app_extension.register_host(host);
#else
    (void)host;
#endif
    return H2_PAL_OK;
}

static h2_pal_result_t route_event(void *user, h2_lua_host_t *host,
                                   h2_lua_job_id_t job,
                                   const h2_runtime_event_t *event,
                                   int *cancel) {
    (void)user;
    if (event->component != H2_RUNTIME_COMPONENT_BUTTON)
        return H2_PAL_OK;
    if (exit_component() != H2_RUNTIME_COMPONENT_ID_NONE &&
        event->component_id == exit_component()) {
        *cancel = exit_requested(event);
        return H2_PAL_OK;
    }
    return h2_lua_dispatch_runtime_event(host, job, event);
}

static h2_pal_result_t run_script(h2_application_host_t *app_host,
                                  h2_runtime_t *runtime, void *user) {
    (void)user;
    static h2_lua_resource_t resource;
    resource = (h2_lua_resource_t){.name = "app.lua",
                                   .source = H2_WEB_LUA_APP_SOURCE,
                                   .source_size = H2_WEB_LUA_APP_SOURCE_SIZE};
    char ids[H2_WEB_APPLICATION_HOST_MAX_BUTTONS][12];
    h2_lua_arg_t
        args[H2_WEB_APPLICATION_HOST_MAX_BUTTONS + H2_WEB_LUA_APP_ARG_COUNT];
    const size_t count = h2_web_application_board.button_count;
    for (size_t i = 0; i < count; ++i) {
        (void)snprintf(ids[i], sizeof(ids[i]), "%u", (unsigned)(i + 1u));
        args[i] = (h2_lua_arg_t){h2_web_application_board.buttons[i].name, ids[i]};
    }
    for (size_t i = 0; i < H2_WEB_LUA_APP_ARG_COUNT; ++i)
        args[count + i] = h2_web_lua_script_args[i];
    const h2_application_host_lua_config_t config = {
        .host = {.worker_count = 1u,
                 .max_jobs = 1u,
                 .event_delivery_capacity = 32u,
                 .callback_capacity_per_job = 16u,
                 .vm_memory_limit_bytes = H2_WEB_LUA_APP_VM_BYTES,
                 .source_limit_bytes = H2_WEB_LUA_APP_SOURCE_LIMIT_BYTES,
                 .output_limit_bytes = 1024u,
                 .instruction_quantum = 10000u,
                 .execution_timeout_ms = UINT32_MAX,
                 .resources = &resource,
                 .resource_count = 1u},
        .resource = "app.lua",
        .args = args,
        .arg_count = count + H2_WEB_LUA_APP_ARG_COUNT,
        .register_host = register_host,
        .event = route_event};
    return h2_application_host_lua_run(app_host, runtime, &config);
}

int main(void) {
    /* Every board Button maps to component id index + 1 under its board name;
     * the board key and skin elements drive it. */
    h2_web_application_host_button_t
        buttons[H2_WEB_APPLICATION_HOST_MAX_BUTTONS];
    const size_t button_count = h2_web_application_board.button_count;
    for (size_t i = 0u; i < button_count; ++i) {
        buttons[i] = (h2_web_application_host_button_t){
            .component_id = (h2_runtime_component_id_t)(i + 1u),
            .key = NULL,
            .name = h2_web_application_board.buttons[i].name,
        };
    }
    const h2_web_application_host_config_t config = {
        .name = H2_WEB_LUA_APP_NAME,
        .buttons = buttons,
        .button_count = button_count,
        .run_ms = H2_WEB_LUA_APP_RUN_MS,
        .stack_size = 262144u,
    };
    return h2_web_application_host_run(&config, run_script, NULL);
}
