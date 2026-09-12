/* App entry for one Lua script, compiled once per h2_lua_web_app().
 *
 * The generated h2_web_lua_app_config.h names the embedded script, the app
 * name, the Buttons and the optional exit Button. app_host assembles the Web
 * Runtime; this entry runs one Lua job, passes every Button component id to
 * the script as `args.<name>`, forwards Button events to the job and ends it
 * from the exit Button or the host stop request. */
#include "h2_web_lua_app_config.h"

#include "h2_lua.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_web_app_host.h"

#if H2_WEB_LUA_APP_EXTENSION
#include "h2_web_lua_app.h"
#endif

#include <stdbool.h>
#include <stdio.h>

typedef struct web_lua_button {
  const char *name;
  const char *key;
} web_lua_button_t;

#define H2_WEB_LUA_BUTTON(name, key) {name, key},
static const web_lua_button_t s_buttons[] = {
    H2_WEB_LUA_APP_BUTTONS(H2_WEB_LUA_BUTTON)};
#undef H2_WEB_LUA_BUTTON

#define BUTTON_COUNT (sizeof(s_buttons) / sizeof(s_buttons[0]))

static bool exit_requested(const h2_runtime_event_t *event) {
#if H2_WEB_LUA_APP_EXTENSION
  if (h2_web_lua_app_extension.exit_requested != NULL)
    return h2_web_lua_app_extension.exit_requested(event);
#endif
  return event->kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
         event->payload_size >= sizeof(h2_runtime_button_action_event_t) &&
         h2_runtime_button_action_is_released(event->payload);
}

static h2_pal_result_t run_script(h2_web_app_host_t *app_host,
                                  h2_runtime_t *runtime, void *user) {
  const h2_lua_resource_t resource = {
      .name = "app.lua",
      .source = H2_WEB_LUA_APP_SOURCE,
      .source_size = H2_WEB_LUA_APP_SOURCE_SIZE,
  };
  char ids[BUTTON_COUNT][12];
  h2_lua_arg_t args[BUTTON_COUNT];
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {.payload = payload,
                              .payload_capacity = sizeof(payload)};
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t job = H2_LUA_JOB_ID_NONE;
  h2_lua_job_status_t status = {0};
  bool ready = false;
  /* Set once a cancel was accepted, so later events do not repeat it. */
  bool cancelling = false;
  (void)user;
  for (size_t i = 0u; i < BUTTON_COUNT; ++i) {
    (void)snprintf(ids[i], sizeof(ids[i]), "%u", (unsigned)(i + 1u));
    args[i] = (h2_lua_arg_t){.name = s_buttons[i].name, .value = ids[i]};
  }
  h2_pal_result_t rc = h2_lua_host_create(
      &(h2_lua_host_config_t){
          .runtime = runtime,
          .worker_count = 1u,
          .max_jobs = 1u,
          .event_delivery_capacity = 32u,
          .callback_capacity_per_job = 16u,
          .vm_memory_limit_bytes = 512u * 1024u,
          .source_limit_bytes = 128u * 1024u,
          .output_limit_bytes = 1024u,
          .instruction_quantum = 10000u,
          .execution_timeout_ms = UINT32_MAX,
          .resources = &resource,
          .resource_count = 1u,
      },
      &host);
  if (rc != H2_PAL_OK)
    return rc;
#if H2_WEB_LUA_APP_EXTENSION
  if (h2_web_lua_app_extension.register_host != NULL)
    rc = h2_web_lua_app_extension.register_host(host);
#endif
  if (rc == H2_PAL_OK)
    rc = h2_lua_host_start(host);
  if (rc == H2_PAL_OK)
    rc = h2_lua_job_submit_resource(host, "app.lua", args, BUTTON_COUNT, &job);
  while (rc == H2_PAL_OK) {
    if (!cancelling && h2_web_app_host_should_stop(app_host)) {
      rc = h2_lua_job_cancel(host, job);
      cancelling = rc == H2_PAL_OK;
    }
    for (; rc == H2_PAL_OK;) {
      const h2_pal_result_t poll = h2_runtime_poll_event(runtime, &event);
      if (poll == H2_PAL_ERR_WOULD_BLOCK || poll == H2_PAL_ERR_TIMEOUT)
        break;
      if (poll != H2_PAL_OK) {
        rc = poll;
      } else if (event.component != H2_RUNTIME_COMPONENT_BUTTON ||
                 cancelling) {
        continue;
      } else if (H2_WEB_LUA_APP_EXIT_BUTTON != 0u &&
                 event.component_id == H2_WEB_LUA_APP_EXIT_BUTTON) {
        if (exit_requested(&event)) {
          rc = h2_lua_job_cancel(host, job);
          cancelling = rc == H2_PAL_OK;
        }
      } else {
        rc = h2_lua_dispatch_runtime_event(host, job, &event);
      }
    }
    if (rc == H2_PAL_OK)
      rc = h2_lua_host_step(host);
    if (rc == H2_PAL_OK)
      rc = h2_lua_job_get_status(host, job, &status);
    if (rc != H2_PAL_OK || status.state >= H2_LUA_JOB_SUCCEEDED)
      break;
    if (!ready && status.state == H2_LUA_JOB_WAITING) {
      ready = true;
      rc = h2_web_app_host_ready(app_host);
      if (rc != H2_PAL_OK)
        break;
    }
    rc = h2_pal_time_sleep_ms(runtime->time, 5u);
  }
  if (job != H2_LUA_JOB_ID_NONE &&
      h2_lua_job_get_status(host, job, &status) == H2_PAL_OK) {
    if (status.state == H2_LUA_JOB_FAILED ||
        status.state == H2_LUA_JOB_TIMED_OUT) {
      printf("H2_WEB_LUA_APP job state=%d message=%s\n", (int)status.state,
             status.message);
      if (rc == H2_PAL_OK)
        rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (status.state >= H2_LUA_JOB_SUCCEEDED)
      (void)h2_lua_job_release(host, job);
  }
  (void)h2_lua_host_stop(host);
  (void)h2_lua_host_join(host);
  h2_lua_host_destroy(host);
  /* A cancelled job (exit Button or Stop) ends the App with OK. */
  return rc;
}

int main(void) {
  h2_web_app_host_button_t buttons[BUTTON_COUNT];
  for (size_t i = 0u; i < BUTTON_COUNT; ++i) {
    /* The page drives each Button from its key and from panel elements
     * marked data-h2-button="<name>". */
    buttons[i] = (h2_web_app_host_button_t){
        .component_id = (h2_runtime_component_id_t)(i + 1u),
        .key = s_buttons[i].key[0] != '\0' ? s_buttons[i].key : NULL,
        .name = s_buttons[i].name,
    };
  }
  const h2_web_app_host_config_t config = {
      .name = H2_WEB_LUA_APP_NAME,
      .display_width = H2_WEB_LUA_APP_DISPLAY_WIDTH,
      .display_height = H2_WEB_LUA_APP_DISPLAY_HEIGHT,
      .buttons = buttons,
      .button_count = BUTTON_COUNT,
      .run_ms = H2_WEB_LUA_APP_RUN_MS,
      .stack_size = 262144u,
  };
  return h2_web_app_host_run(&config, run_script, NULL);
}
