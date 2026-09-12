#include "h2_lua_fishing_game.h"

#include "fishing_game_script_generated.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2_lua.h"
#include "h2_lua_module.h"
#include "h2_lua_fishing_math.h"
#include "h2_lua_event.h"
#include "h2_runtime_test.h"
#include "lua.h"
#include "lauxlib.h"
#include "h2_lua_job.h"

#include <stdio.h>

typedef struct {
  h2_runtime_t *runtime;
  h2_runtime_test_control_t *control;
  uint64_t pressed[5];
} fishing_input_test_t;

static int test_button(lua_State *s) {
  fishing_input_test_t *test = lua_touserdata(s, lua_upvalueindex(1));
  int id = (int)luaL_checkinteger(s, 1);
  int down = lua_toboolean(s, 2);
  if (id < 9 || id > 13) return luaL_error(s, "invalid test button");
  h2_pal_result_t rc = H2_PAL_OK;
  if (!test->control) rc = h2_runtime_test_control_open(test->runtime, &test->control);
  uint64_t now = 0;
  if (rc == H2_PAL_OK) rc = h2_pal_time_get_monotonic_ms(test->runtime->time, &now);
  if (rc == H2_PAL_OK && down) {
    test->pressed[id-9] = now;
    rc = h2_runtime_test_button_down(test->control, (h2_runtime_component_id_t)id, now);
  } else if (rc == H2_PAL_OK) {
    rc = h2_runtime_test_button_up(test->control, (h2_runtime_component_id_t)id, test->pressed[id-9], now);
    test->pressed[id-9] = 0;
  }
  if (rc != H2_PAL_OK) return luaL_error(s, "Runtime test button failed: %d", rc);
  return 0;
}

static int test_finish(lua_State *s) {
  fishing_input_test_t *test = lua_touserdata(s, lua_upvalueindex(1));
  if (test->control) h2_runtime_test_control_close(test->control);
  test->control = NULL;
  return 0;
}

static int input_test_open(void *opaque, void *user) {
  lua_State *s = opaque;
  lua_newtable(s);
  lua_pushlightuserdata(s, user);lua_pushcclosure(s, test_button, 1);lua_setfield(s, -2, "button");
  lua_pushlightuserdata(s, user);lua_pushcclosure(s, test_finish, 1);lua_setfield(s, -2, "finish");
  return 1;
}

static int terminal(h2_lua_job_state_t state) {
  return state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
         state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
         state == H2_LUA_JOB_STOPPED;
}

static int supported_event(h2_runtime_event_kind_t kind) {
  return kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN ||
         kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP ||
         kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION ||
         kind == H2_RUNTIME_COMPONENT_EVENT_NFC_STATE ||
         kind == H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE ||
         kind == H2_RUNTIME_COMPONENT_EVENT_ERROR;
}

h2_pal_result_t
h2_lua_fishing_game_run(h2_runtime_t *runtime,
                      const h2_lua_fishing_game_config_t *config) {
  h2_lua_host_t *host = NULL;
  fishing_input_test_t input_test = {.runtime = runtime};
  h2_lua_job_id_t job_id = H2_LUA_JOB_ID_NONE;
  h2_lua_job_status_t status;
  h2_pal_result_t result;
  int ready_reported = 0;
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
  const h2_lua_resource_t resources[] = {
      {
          .name = "@fishing/main.lua",
          .source = fishing_game_script,
          .source_size = fishing_game_script_size,
      },
  };

  if (runtime == NULL || config == NULL || config->should_stop == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  const h2_lua_arg_t args[] = {
      {.name = "profile", .value = config->profile != NULL ? config->profile : "desktop"},
      {.name = "weather", .value = config->weather ? config->weather : "auto"},
      {.name = "hour", .value = config->hour ? config->hour : "9"},
      {.name = "scene", .value = config->scene ? config->scene : "idle"},
      {.name = "time_ms", .value = config->time_ms ? config->time_ms : ""},
      {.name = "rod", .value = config->rod ? config->rod : "1"},
      {.name = "reel", .value = config->reel ? config->reel : "1"},
      {.name = "lure", .value = config->lure ? config->lure : "1"},
      {.name = "detail", .value = config->detail ? config->detail : "0"},
      {.name = "fish_kg", .value = config->fish_kg ? config->fish_kg : "2"},
      {.name = "power", .value = config->power ? config->power : "ML"},
      {.name = "action", .value = config->action ? config->action : "F"},
      {.name = "brand", .value = config->brand ? config->brand : "1"},
      {.name = "check", .value = config->check ? config->check : "0"},
      {.name = "no_cache", .value = config->no_cache ? config->no_cache : "0"},
      {.name = "scroll", .value = config->scroll ? config->scroll : ""},
      {.name = "layout", .value = config->layout ? config->layout : "amoled"},
      {.name = "controls", .value = config->controls ? config->controls : "touch"},
      {.name = "input_test", .value = config->input_test ? "1" : "0"},
  };

  result = h2_lua_host_create(
      &(h2_lua_host_config_t){
          .runtime = runtime,
          .worker_count = 1u,
          .max_jobs = 1u,
          .event_delivery_capacity = 32u,
          .callback_capacity_per_job = 16u,
          .vm_memory_limit_bytes = config->vm_memory_limit_bytes ? config->vm_memory_limit_bytes : 4u * 1024u * 1024u,
          .source_limit_bytes = 256u * 1024u,
          .output_limit_bytes = 1024u,
          .instruction_quantum = 50000u,
          .execution_timeout_ms = UINT32_MAX,
          .resources = resources,
          .resource_count = sizeof(resources) / sizeof(resources[0]),
      },
      &host);
  if (result != H2_PAL_OK) {
    return result;
  }
  result = h2_lua_register_module(host, "fishing_math", h2_lua_fishing_math_open, runtime);
  if (result == H2_PAL_OK && config->input_test)
    result = h2_lua_register_module(host, "fishing_test", input_test_open, &input_test);
  if (result == H2_PAL_OK) result = h2_lua_host_start(host);
  if (result == H2_PAL_OK) {
    result =
        h2_lua_job_submit_resource(host, "@fishing/main.lua", args,
                                   sizeof(args) / sizeof(args[0]), &job_id);
  }
  while (result == H2_PAL_OK) {
    if (config->should_stop(config->should_stop_user)) {
      (void)h2_lua_job_cancel(host, job_id);
    }
    for (;;) {
      h2_pal_result_t poll_result = h2_runtime_poll_event(runtime, &event);
      if (poll_result == H2_PAL_ERR_WOULD_BLOCK ||
          poll_result == H2_PAL_ERR_TIMEOUT) {
        break;
      }
      if (poll_result != H2_PAL_OK) {
        result = poll_result;
        break;
      }
      if (config->back_component_id != H2_RUNTIME_COMPONENT_ID_NONE &&
          event.component_id == config->back_component_id &&
          event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
          event.payload_size >= sizeof(h2_runtime_button_action_event_t) &&
          h2_runtime_button_action_is_released(event.payload)) {
        (void)h2_lua_job_cancel(host, job_id);
      } else if (supported_event(event.kind)) {
        result = h2_lua_dispatch_runtime_event(host, job_id, &event);
        if (result != H2_PAL_OK) {
          break;
        }
      }
    }
    if (result == H2_PAL_OK) {
      result = h2_lua_host_step(host);
    }
    if (result != H2_PAL_OK ||
        h2_lua_job_get_status(host, job_id, &status) != H2_PAL_OK ||
        terminal(status.state)) {
      break;
    }
    if (!ready_reported && status.state == H2_LUA_JOB_WAITING) {
      ready_reported = 1;
      if (config->on_ready != NULL) {
        result = config->on_ready(config->on_ready_user);
        if (result != H2_PAL_OK) {
          (void)h2_lua_job_cancel(host, job_id);
          break;
        }
      }
    }
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  if (job_id != H2_LUA_JOB_ID_NONE &&
      h2_lua_job_get_status(host, job_id, &status) == H2_PAL_OK) {
    if (terminal(status.state)) {
      char diagnostic[320];
      (void)snprintf(diagnostic, sizeof(diagnostic),
                     "terminal state=%d resumes=%llu memory=%zu message=%s",
                     (int)status.state, (unsigned long long)status.resume_count,
                     status.memory_used, status.message);
      (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "lua-fishing-game",
                             diagnostic);
    }
    if (status.state == H2_LUA_JOB_FAILED ||
        status.state == H2_LUA_JOB_TIMED_OUT) {
      result = H2_PAL_ERR_INVALID_STATE;
    } else if (status.state == H2_LUA_JOB_CANCELLED) {
      result = H2_PAL_OK;
    }
    if (terminal(status.state)) {
      (void)h2_lua_job_release(host, job_id);
    }
  }
  h2_lua_host_destroy(host);
  if (input_test.control) h2_runtime_test_control_close(input_test.control);
  return result;
}
