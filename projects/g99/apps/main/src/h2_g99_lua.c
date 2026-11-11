#include "h2_g99_internal.h"
#include "h2_lua_job.h"
#include "orbit_script_generated.h"

#include <stdio.h>

#define H2_G99_ORBIT_RESOURCE "@g99/orbit.lua"
#define H2_G99_ORBIT_APP_ID "g99.orbit"
#define H2_G99_DESTROY_ATTEMPTS 400u

static bool terminal(h2_lua_job_state_t state) {
  return state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
         state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
         state == H2_LUA_JOB_STOPPED;
}

static h2_pal_result_t destroy_host(h2_g99_app_t* app) {
  h2_pal_result_t rc = H2_PAL_OK;
  for (uint32_t attempt = 0u; app->host != NULL; ++attempt) {
    rc = h2_lua_host_destroy_checked(app->host);
    if (rc == H2_PAL_OK) {
      app->host = NULL;
      break;
    }
    if (rc != H2_PAL_ERR_BUSY || attempt + 1u >= H2_G99_DESTROY_ATTEMPTS) break;
    (void)h2_pal_time_sleep_ms(app->runtime->time, 5u);
  }
  app->job = H2_LUA_JOB_ID_NONE;
  return rc;
}

h2_pal_result_t h2_g99_lua_launch(h2_g99_app_t* app) {
  if (app == NULL || app->host != NULL) return H2_PAL_ERR_INVALID_STATE;
  const h2_lua_resource_t resources[] = {
      {H2_G99_ORBIT_RESOURCE, h2_g99_orbit_script, h2_g99_orbit_script_size},
  };
  h2_lua_host_config_t config = {0};
  config.runtime = app->runtime;
  config.worker_count = 1u;
  config.worker_stack_size = 64u * 1024u;
  config.max_jobs = 1u;
  config.event_delivery_capacity = 8u;
  config.callback_capacity_per_job = 8u;
  config.vm_memory_limit_bytes = 1024u * 1024u;
  config.source_limit_bytes = 64u * 1024u;
  config.output_limit_bytes = 1024u;
  config.instruction_quantum = 10000u;
  config.execution_timeout_ms = UINT32_MAX;
  config.resources = resources;
  config.resource_count = sizeof(resources) / sizeof(resources[0]);
  /* Main keeps the panel open; the app draws into it while LVGL is paused. */
  config.borrow_display = 1;
  config.display_framebuffer_bytes =
      (size_t)app->layout.width * (size_t)app->layout.height * sizeof(uint16_t);
  h2_g99_ui_suspend(app);
  h2_pal_result_t rc = h2_lua_host_create(&config, &app->host);
  if (rc == H2_PAL_OK) rc = h2_lua_host_start(app->host);
  char ids[3][12];
  const unsigned components[3] = {H2_G99_COMPONENT_UP, H2_G99_COMPONENT_DOWN,
                                  H2_G99_COMPONENT_OK};
  for (int i = 0; i < 3; ++i)
    (void)snprintf(ids[i], sizeof(ids[i]), "%u", components[i]);
  const h2_lua_arg_t args[] = {
      {"up", ids[0]},
      {"down", ids[1]},
      {"ok", ids[2]},
  };
  if (rc == H2_PAL_OK)
    rc = h2_lua_job_submit_resource(app->host, H2_G99_ORBIT_APP_ID,
                                    H2_G99_ORBIT_RESOURCE, args,
                                    sizeof(args) / sizeof(args[0]), &app->job);
  if (rc != H2_PAL_OK) {
    h2_g99_log(app, "lua launch failed rc=%d", (int)rc);
    const h2_pal_result_t cleanup = destroy_host(app);
    if (cleanup != H2_PAL_OK) return cleanup;
    h2_g99_ui_resume(app);
    return rc;
  }
  app->lifecycle = H2_G99_LIFECYCLE_APP_RUNNING;
  h2_g99_log(app, "lua app started id=%s", H2_G99_ORBIT_APP_ID);
  return H2_PAL_OK;
}

bool h2_g99_lua_poll(h2_g99_app_t* app) {
  if (app == NULL || app->host == NULL) return false;
  h2_pal_result_t rc = h2_lua_host_step(app->host);
  h2_lua_job_status_t status;
  if (rc == H2_PAL_OK) rc = h2_lua_job_get_status(app->host, app->job, &status);
  if (rc != H2_PAL_OK) {
    h2_g99_log(app, "lua app poll failed rc=%d", (int)rc);
    return false;
  }
  if (!terminal(status.state)) return true;
  h2_g99_log(app, "lua app ended state=%d message=%s", (int)status.state,
             status.message);
  return false;
}

h2_pal_result_t h2_g99_lua_stop(h2_g99_app_t* app) {
  if (app == NULL || app->host == NULL) return H2_PAL_OK;
  if (app->job != H2_LUA_JOB_ID_NONE)
    (void)h2_lua_job_cancel(app->host, app->job);
  const h2_pal_result_t rc = destroy_host(app);
  if (rc != H2_PAL_OK) {
    h2_g99_log(app, "lua host release failed rc=%d", (int)rc);
    return rc;
  }
  app->lifecycle = H2_G99_LIFECYCLE_RUNNING;
  h2_g99_ui_resume(app);
  h2_g99_log(app, "lua app stopped");
  return H2_PAL_OK;
}
