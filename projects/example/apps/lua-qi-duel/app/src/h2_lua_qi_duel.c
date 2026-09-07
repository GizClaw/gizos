#include "h2_lua_qi_duel.h"

#include "h2/pal/os/h2_pal_log.h"
#include "h2_lua.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "qi_duel_assets.h"
#include "qi_duel_script_generated.h"

#include <stdio.h>

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

h2_pal_result_t h2_lua_qi_duel_run(h2_runtime_t *runtime,
                                   const h2_lua_qi_duel_config_t *config) {
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t job_id = H2_LUA_JOB_ID_NONE;
  h2_lua_job_status_t status;
  h2_pal_result_t result;
  int ready_reported = 0;
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
#define H2_QI_DUEL_ASSET(asset_name, symbol)                                   \
  {                                                                            \
    .name = "@qi-duel/" asset_name ".a4", .source = symbol,                    \
    .source_size = symbol##_size,                                              \
  }
  const h2_lua_resource_t resources[] = {
      {.name = "@qi-duel/main.lua",
       .source = qi_duel_script,
       .source_size = qi_duel_script_size},
      H2_QI_DUEL_ASSET("player-hud", h2_qi_duel_asset_player_hud),
      H2_QI_DUEL_ASSET("enemy-hud", h2_qi_duel_asset_enemy_hud),
      H2_QI_DUEL_ASSET("opponent", h2_qi_duel_asset_opponent),
      H2_QI_DUEL_ASSET("hand-left", h2_qi_duel_asset_hand_left),
      H2_QI_DUEL_ASSET("hand-right", h2_qi_duel_asset_hand_right),
      H2_QI_DUEL_ASSET("carousel", h2_qi_duel_asset_carousel),
      H2_QI_DUEL_ASSET("skill-charge", h2_qi_duel_asset_skill_charge),
      H2_QI_DUEL_ASSET("skill-charge-left", h2_qi_duel_asset_skill_charge_left),
      H2_QI_DUEL_ASSET("skill-charge-right",
                       h2_qi_duel_asset_skill_charge_right),
      H2_QI_DUEL_ASSET("skill-wave", h2_qi_duel_asset_skill_wave),
      H2_QI_DUEL_ASSET("skill-wave-left", h2_qi_duel_asset_skill_wave_left),
      H2_QI_DUEL_ASSET("skill-wave-right", h2_qi_duel_asset_skill_wave_right),
      H2_QI_DUEL_ASSET("skill-absorb", h2_qi_duel_asset_skill_absorb),
      H2_QI_DUEL_ASSET("skill-absorb-left", h2_qi_duel_asset_skill_absorb_left),
      H2_QI_DUEL_ASSET("skill-absorb-right",
                       h2_qi_duel_asset_skill_absorb_right),
      H2_QI_DUEL_ASSET("skill-guard", h2_qi_duel_asset_skill_guard),
      H2_QI_DUEL_ASSET("skill-guard-left", h2_qi_duel_asset_skill_guard_left),
      H2_QI_DUEL_ASSET("skill-guard-right", h2_qi_duel_asset_skill_guard_right),
  };
#undef H2_QI_DUEL_ASSET
  if (runtime == NULL || config == NULL || config->should_stop == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
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
          .output_limit_bytes = 8u * 1024u,
          .instruction_quantum = 10000u,
          .execution_timeout_ms = UINT32_MAX,
          .resources = resources,
          .resource_count = sizeof(resources) / sizeof(resources[0]),
      },
      &host);
  if (result != H2_PAL_OK) {
    return result;
  }
  result = h2_lua_host_start(host);
  if (result == H2_PAL_OK) {
    result = h2_lua_job_submit_resource(host, "@qi-duel/main.lua", NULL, 0u,
                                        &job_id);
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
      (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "lua-qi-duel",
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
  return result;
}
