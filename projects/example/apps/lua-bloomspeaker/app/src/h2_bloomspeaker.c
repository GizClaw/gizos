#include "h2_bloomspeaker.h"

#include "h2_bloomspeaker_controller.h"
#include "h2_bloomspeaker_engine.h"
#include "h2_bloomspeaker_lua.h"

#include "lua_bloomspeaker_script_generated.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2_lua.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_lua_module.h"

#include <stdio.h>
#include <stdatomic.h>

#define H2_BLOOMSPEAKER_POWER_HOLD_MS 2000u
#define H2_BLOOMSPEAKER_PAIR_HOLD_MS 1000u
#define H2_BLOOMSPEAKER_SHUTDOWN_FADE_TIMEOUT_MS 1000u

static int terminal(h2_lua_job_state_t state) {
  return state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
         state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
         state == H2_LUA_JOB_STOPPED;
}

static int supported_event(h2_runtime_event_kind_t kind) {
  return kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN ||
         kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP ||
         kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION ||
         kind == H2_RUNTIME_COMPONENT_EVENT_ERROR;
}

static h2_pal_result_t poll_button_state(
    h2_runtime_t *runtime, h2_runtime_component_id_t component_id,
    h2_runtime_button_state_t *out_state) {
  if (component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  h2_runtime_button_state_t state = {0};
  h2_pal_result_t result = h2_runtime_component_state_button(
      runtime, component_id, &state);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (state.result != H2_PAL_OK) {
    return state.result;
  }
  if (state.updated_at_ms == 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *out_state = state;
  return H2_PAL_OK;
}

static void report_pair_button(h2_bloomspeaker_controller_t *controller,
                               const char *action) {
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(controller, &snapshot);
  printf("H2_BLOOMSPEAKER_BUTTON component=pair action=%s state=%s\n",
         action, h2_bloomspeaker_state_name(snapshot.state));
}

h2_pal_result_t h2_bloomspeaker_run(
    h2_runtime_t *runtime,
    const h2_bloomspeaker_config_t *config) {
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t job_id = H2_LUA_JOB_ID_NONE;
  h2_lua_job_status_t status;
  h2_pal_result_t result;
  uint64_t started_ms = 0u;
  h2_bloomspeaker_controller_t controller;
  h2_bloomspeaker_hold_tracker_t power_hold = {0};
  h2_bloomspeaker_hold_tracker_t pairing_hold = {0};
  h2_bloomspeaker_engine_t *engine = NULL;
  _Atomic bool shutdown_fade_requested = false;
  h2_bloomspeaker_lua_context_t lua_context = {
      .runtime = runtime,
      .controller = &controller,
      .shutdown_requested = &shutdown_fade_requested,
  };
  int ready_reported = 0;
  int shutdown_requested = 0;
  uint64_t shutdown_deadline_ms = 0u;
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
  const h2_lua_resource_t resources[] = {
      {
          .name = "@lua-bloomspeaker/main.lua",
          .source = lua_bloomspeaker_script,
          .source_size = lua_bloomspeaker_script_size,
      },
  };
  if (runtime == NULL || config == NULL || config->should_stop == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  lua_context.touch_pairing_enabled =
      config->pairing_component_id == H2_RUNTIME_COMPONENT_ID_NONE;
  (void)h2_pal_time_get_monotonic_ms(runtime->time, &started_ms);
  h2_bloomspeaker_controller_init(&controller, started_ms);
  result = h2_bloomspeaker_engine_start(
      runtime, &controller,
      &(h2_bloomspeaker_engine_config_t){
          .pause_management_advertising =
              config->pause_management_advertising,
          .resume_management_advertising =
              config->resume_management_advertising,
          .management_advertising_user =
              config->management_advertising_user,
      },
      &engine);
  if (result != H2_PAL_OK) {
    return result;
  }
  result = h2_lua_host_create(
      &(h2_lua_host_config_t){
          .runtime = runtime,
          .worker_count = 1u,
          .max_jobs = 1u,
          .event_delivery_capacity = 8u,
          .callback_capacity_per_job = 8u,
          .vm_memory_limit_bytes = 512u * 1024u,
          .source_limit_bytes = 128u * 1024u,
          .output_limit_bytes = 1024u,
          .instruction_quantum = 10000u,
          .execution_timeout_ms = UINT32_MAX,
          .resources = resources,
          .resource_count = sizeof(resources) / sizeof(resources[0]),
      },
      &host);
  if (result != H2_PAL_OK) {
    (void)h2_bloomspeaker_engine_stop(engine);
    return result;
  }
  result = h2_lua_register_module(host, "intercom", h2_bloomspeaker_lua_open,
                                  &lua_context);
  if (result == H2_PAL_OK) {
    result = h2_lua_host_start(host);
  }
  if (result == H2_PAL_OK) {
    result = h2_lua_job_submit_resource(
        host, "@lua-bloomspeaker/main.lua", NULL, 0u, &job_id);
  }
  while (result == H2_PAL_OK) {
    uint64_t now_ms = 0u;
    result = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    if (result != H2_PAL_OK) {
      break;
    }
    if (config->should_stop(config->should_stop_user)) {
      (void)h2_lua_job_cancel(host, job_id);
    }
    if (shutdown_requested && now_ms >= shutdown_deadline_ms) {
      printf("H2_BLOOMSPEAKER_POWER action=fade_timeout\n");
      (void)h2_lua_job_cancel(host, job_id);
    }
    h2_runtime_button_state_t power_state = {0};
    h2_pal_result_t power_result = poll_button_state(
        runtime, config->power_component_id, &power_state);
    if (power_result == H2_PAL_OK) {
      if (h2_bloomspeaker_hold_tracker_update(
              &power_hold, power_state.pressed, now_ms,
              H2_BLOOMSPEAKER_POWER_HOLD_MS)) {
        if (!shutdown_requested) {
          shutdown_requested = 1;
          shutdown_deadline_ms =
              now_ms + H2_BLOOMSPEAKER_SHUTDOWN_FADE_TIMEOUT_MS;
          atomic_store_explicit(&shutdown_fade_requested, true,
                                memory_order_release);
          printf("H2_BLOOMSPEAKER_POWER action=fade_requested\n");
        }
      }
    } else if (power_result != H2_PAL_ERR_WOULD_BLOCK &&
               power_result != H2_PAL_ERR_NOT_FOUND) {
      power_hold = (h2_bloomspeaker_hold_tracker_t){0};
    }
    h2_runtime_button_state_t pairing_state = {0};
    h2_pal_result_t pairing_result = poll_button_state(
        runtime, config->pairing_component_id, &pairing_state);
    if (!shutdown_requested && pairing_result == H2_PAL_OK) {
      const int released_after_trigger =
          pairing_hold.pressed && pairing_hold.triggered &&
          !pairing_state.pressed;
      if (h2_bloomspeaker_hold_tracker_update(
              &pairing_hold, pairing_state.pressed, now_ms,
              H2_BLOOMSPEAKER_PAIR_HOLD_MS)) {
        h2_bloomspeaker_controller_long_press(&controller, now_ms);
        report_pair_button(&controller, "hold");
      } else if (released_after_trigger) {
        h2_bloomspeaker_controller_hold_release(&controller, now_ms);
        report_pair_button(&controller, "release");
      }
    } else if (!shutdown_requested &&
               pairing_result != H2_PAL_ERR_WOULD_BLOCK &&
               pairing_result != H2_PAL_ERR_NOT_FOUND) {
      pairing_hold = (h2_bloomspeaker_hold_tracker_t){0};
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
      } else if (event.component_id == config->power_component_id ||
                 event.component_id == config->pairing_component_id) {
        /* Native hold trackers own these buttons. Runtime events still need
         * draining, but Lua has no callback registered for either component. */
        continue;
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
      (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO,
                             "lua-bloomspeaker", diagnostic);
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
  h2_pal_result_t engine_result = h2_bloomspeaker_engine_stop(engine);
  if (result == H2_PAL_OK) {
    result = engine_result;
  }
  if (shutdown_requested) {
    h2_pal_result_t shutdown_result =
        h2_pal_power_shutdown(runtime->power, 0u);
    printf("H2_BLOOMSPEAKER_POWER action=shutdown rc=%d\n",
           (int)shutdown_result);
    if (result == H2_PAL_OK) {
      result = shutdown_result;
    }
  }
  return result;
}
