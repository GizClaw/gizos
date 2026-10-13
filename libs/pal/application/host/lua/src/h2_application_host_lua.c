#include "h2_application_host_lua.h"
#include "h2_lua_event.h"

#include <stdbool.h>
#include <stdio.h>

static h2_pal_result_t
dispatch_component_event(h2_lua_host_t *lua, h2_lua_job_id_t job,
                         const h2_runtime_event_t *event) {
    switch (event->kind) {
    case H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN:
    case H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP:
    case H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION:
    case H2_RUNTIME_COMPONENT_EVENT_NFC_STATE:
    case H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE:
    case H2_RUNTIME_COMPONENT_EVENT_ERROR:
        break;
    default:
        // System/custom events have no Lua component identity. Products can
        // handle them through the explicit event callback without failing a
        // job.
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_lua_dispatch_runtime_event(lua, job, event);
    if (rc == H2_PAL_ERR_CLOSED) {
        h2_lua_job_status_t status;
        if (h2_lua_job_get_status(lua, job, &status) == H2_PAL_OK &&
            status.state >= H2_LUA_JOB_SUCCEEDED)
            return H2_PAL_OK;
    }
    return rc;
}

h2_pal_result_t
h2_application_host_lua_run(h2_application_host_t *outer, h2_runtime_t *runtime,
                            const h2_application_host_lua_config_t *config) {
    if (outer == NULL || runtime == NULL || config == NULL ||
        config->resource == NULL ||
        (config->arg_count != 0 && config->args == NULL))
        return H2_PAL_ERR_INVALID_ARG;
    h2_lua_host_config_t selected = config->host;
    selected.runtime = runtime;
    h2_lua_host_t *lua = NULL;
    h2_pal_result_t rc = h2_lua_host_create(&selected, &lua);
    if (rc != H2_PAL_OK)
        return rc;
    if (config->register_host != NULL)
        rc = config->register_host(config->user, lua);
    if (rc == H2_PAL_OK)
        rc = h2_lua_host_start(lua);
    h2_lua_job_id_t job = H2_LUA_JOB_ID_NONE;
    h2_lua_job_status_t status = {0};
    if (rc == H2_PAL_OK)
        rc = h2_lua_job_submit_resource(lua, NULL, config->resource,
                                        config->args, config->arg_count, &job);
    bool cancelling = false, ready = false;
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {.payload = payload,
                                .payload_capacity = sizeof(payload)};
    while (rc == H2_PAL_OK) {
        if (!cancelling && h2_application_host_should_stop(outer)) {
            rc = h2_lua_job_cancel(lua, job);
            cancelling = rc == H2_PAL_OK;
        }
        for (unsigned i = 0; rc == H2_PAL_OK && i < 64; ++i) {
            h2_pal_result_t polled = h2_runtime_poll_event(runtime, &event);
            if (polled == H2_PAL_ERR_WOULD_BLOCK ||
                polled == H2_PAL_ERR_TIMEOUT)
                break;
            if (polled != H2_PAL_OK) {
                rc = polled;
                break;
            }
            if (cancelling)
                continue;
            int cancel = 0;
            rc = config->event != NULL
                     ? config->event(config->user, lua, job, &event, &cancel)
                     : dispatch_component_event(lua, job, &event);
            if (rc == H2_PAL_OK && cancel) {
                rc = h2_lua_job_cancel(lua, job);
                cancelling = rc == H2_PAL_OK;
            }
        }
        if (rc == H2_PAL_OK)
            rc = h2_lua_host_step(lua);
        if (rc == H2_PAL_OK)
            rc = h2_lua_job_get_status(lua, job, &status);
        if (rc != H2_PAL_OK || status.state >= H2_LUA_JOB_SUCCEEDED)
            break;
        if (!ready && status.state == H2_LUA_JOB_WAITING) {
            ready = true;
            rc = h2_application_host_ready(outer);
        }
        if (rc == H2_PAL_OK)
            rc = h2_pal_time_sleep_ms(runtime->time, 5u);
    }
    if (job != H2_LUA_JOB_ID_NONE &&
        h2_lua_job_get_status(lua, job, &status) == H2_PAL_OK) {
        if (status.state == H2_LUA_JOB_FAILED ||
            status.state == H2_LUA_JOB_TIMED_OUT) {
            printf("H2_APPLICATION_LUA job state=%d message=%s\n",
                   (int)status.state, status.message);
            if (rc == H2_PAL_OK)
                rc = H2_PAL_ERR_INVALID_STATE;
        }
        if (status.state >= H2_LUA_JOB_SUCCEEDED) {
            h2_pal_result_t released = h2_lua_job_release(lua, job);
            // A canceled job may still have a display submission in flight.
            // Checked Host destruction below owns its drain and final release.
            if (rc == H2_PAL_OK && released != H2_PAL_ERR_BUSY)
                rc = released;
        }
    }
    h2_pal_result_t cleanup = h2_lua_host_destroy_checked(lua);
    while (cleanup == H2_PAL_ERR_BUSY) {
        h2_pal_result_t slept = h2_pal_time_sleep_ms(runtime->time, 1u);
        if (slept != H2_PAL_OK) {
            cleanup = slept;
            break;
        }
        cleanup = h2_lua_host_destroy_checked(lua);
    }
    if (cleanup != H2_PAL_OK) {
        (void)h2_application_host_retain(outer, lua, cleanup);
        return cleanup;
    }
    return rc;
}
