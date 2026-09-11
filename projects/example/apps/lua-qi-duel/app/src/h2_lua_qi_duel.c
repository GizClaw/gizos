#include "h2_lua_qi_duel.h"
#ifdef H2_QI_DUEL_DESKTOP_VECTORS
#include "qi_duel_procedural_generated.h"
#include "vector_opponent_generated.h"
#include "vector_left_generated.h"
#include "vector_right_generated.h"
#include "vector_components_generated.h"
#include "qi_duel_component_paths_generated.h"
#include "qi_duel_component_renderer_generated.h"
#include "vector_charge_generated.h"
#include "vector_wave_generated.h"
#include "vector_absorb_generated.h"
#include "vector_guard_generated.h"
#endif

#include "h2/pal/os/h2_pal_log.h"
#include "h2_lua.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_lua_module.h"
#include "qi_duel_link.h"
#include "qi_duel_script_generated.h"
#include "qi_duel_retro_audio_generated.h"
#include "qi_duel_retro_score_generated.h"
#include "qi_duel_rules_generated.h"
#include "qi_duel_link_protocol_generated.h"
#ifndef H2_QI_DUEL_VECTOR_ONLY
#include "countdown_rgba_generated.h"
#include "impact_labels_rgba_generated.h"
#include "wall_atlas_generated.h"
#include "arena_atlas_generated.h"
#include "arena_full_atlas_generated.h"
#include "opponent_rgba_generated.h"
#include "left_rgba_generated.h"
#include "right_rgba_generated.h"
#include "hud_rgba_generated.h"
#include "carousel_rgba_generated.h"
#include "charge_rgba_generated.h"
#include "skill_styles_generated.h"
#include "skill_colors_generated.h"
#include "action_opponent_generated.h"
#include "action_hands_generated.h"
#include "beam_clash_h106_generated.h"
#include "beam_clash_amoled_generated.h"
#include "beam_clash_fade_h106_generated.h"
#include "beam_clash_fade_amoled_generated.h"
#include "result_words_generated.h"
#include "result_streaks_generated.h"
#endif

#include <stdio.h>
#include <string.h>

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
  int event_pending = 0;
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
  const h2_lua_resource_t resources[] = {
      {.name = "@qi-duel/retro_audio.lua", .source = qi_duel_retro_audio, .source_size = qi_duel_retro_audio_size},
      {.name = "@qi-duel/retro_score.lua", .source = qi_duel_retro_score, .source_size = qi_duel_retro_score_size},
      {.name = "@qi-duel/main.lua",
       .source = qi_duel_script,
       .source_size = qi_duel_script_size},
      {.name = "@qi-duel/rules.lua", .source = qi_duel_rules,
       .source_size = qi_duel_rules_size},
      {.name = "@qi-duel/link_protocol.lua", .source = qi_duel_link_protocol,
       .source_size = qi_duel_link_protocol_size},
#ifndef H2_QI_DUEL_VECTOR_ONLY
      {.name = "@qi-duel/countdown.h2r8", .source = qi_duel_countdown_rgba,
       .source_size = qi_duel_countdown_rgba_size},
      {.name = "@qi-duel/impact-labels.h2r8",
       .source = qi_duel_impact_labels_rgba,
       .source_size = qi_duel_impact_labels_rgba_size},
      {.name = "@qi-duel/action-opponent.h2rs", .source = qi_duel_action_opponent,
       .source_size = qi_duel_action_opponent_size},
      {.name = "@qi-duel/action-hands.h2rs", .source = qi_duel_action_hands,
       .source_size = qi_duel_action_hands_size},
      {.name = "@qi-duel/beam-clash-h106.h2rs",
       .source = qi_duel_beam_clash_h106,
       .source_size = qi_duel_beam_clash_h106_size},
      {.name = "@qi-duel/beam-clash-amoled.h2rs",
       .source = qi_duel_beam_clash_amoled,
       .source_size = qi_duel_beam_clash_amoled_size},
      {.name = "@qi-duel/beam-clash-fade-h106.h2rs",
       .source = qi_duel_beam_clash_fade_h106,
       .source_size = qi_duel_beam_clash_fade_h106_size},
      {.name = "@qi-duel/beam-clash-fade-amoled.h2rs",
       .source = qi_duel_beam_clash_fade_amoled,
       .source_size = qi_duel_beam_clash_fade_amoled_size},
      {.name = "@qi-duel/result-words.h2rs",
       .source = qi_duel_result_words,
       .source_size = qi_duel_result_words_size},
      {.name = "@qi-duel/result-streaks.h2r8",
       .source = qi_duel_result_streaks,
       .source_size = qi_duel_result_streaks_size},
      {.name = "@qi-duel/wall-lights.h2lf",
       .source = qi_duel_wall_atlas,
       .source_size = qi_duel_wall_atlas_size},
      {.name = "@qi-duel/arena-lights.h2lf",
       .source = qi_duel_arena_atlas,
       .source_size = qi_duel_arena_atlas_size},
      {.name = "@qi-duel/arena-lights-full.h2lf",
       .source = qi_duel_arena_full_atlas,
       .source_size = qi_duel_arena_full_atlas_size},
#endif
#ifdef H2_QI_DUEL_DESKTOP_VECTORS
      {.name = "@qi-duel/vector/components.h2vp", .source = qi_duel_vector_components, .source_size = qi_duel_vector_components_size},
      {.name = "@qi-duel/component_paths.lua", .source = qi_duel_component_paths, .source_size = qi_duel_component_paths_size},
      {.name = "@qi-duel/component_renderer.lua", .source = qi_duel_component_renderer, .source_size = qi_duel_component_renderer_size},
      {.name = "@qi-duel/procedural.lua", .source = qi_duel_procedural, .source_size = qi_duel_procedural_size},
      {.name = "@qi-duel/vector/hand-right.h2vg", .source = qi_duel_vector_right, .source_size = qi_duel_vector_right_size},
      {.name = "@qi-duel/vector/hand-left.h2vg", .source = qi_duel_vector_left, .source_size = qi_duel_vector_left_size},
      {.name = "@qi-duel/vector/opponent.h2vg", .source = qi_duel_vector_opponent, .source_size = qi_duel_vector_opponent_size},
      {.name = "@qi-duel/vector/charge.h2vg", .source = qi_duel_vector_charge, .source_size = qi_duel_vector_charge_size},
      {.name = "@qi-duel/vector/wave.h2vg", .source = qi_duel_vector_wave, .source_size = qi_duel_vector_wave_size},
      {.name = "@qi-duel/vector/absorb.h2vg", .source = qi_duel_vector_absorb, .source_size = qi_duel_vector_absorb_size},
      {.name = "@qi-duel/vector/guard.h2vg", .source = qi_duel_vector_guard, .source_size = qi_duel_vector_guard_size},
#endif
#ifndef H2_QI_DUEL_VECTOR_ONLY
      {.name = "@qi-duel/opponent.h2r8", .source = qi_duel_opponent_rgba,
       .source_size = qi_duel_opponent_rgba_size},
      {.name = "@qi-duel/hand-left.h2r8", .source = qi_duel_left_rgba,
       .source_size = qi_duel_left_rgba_size},
      {.name = "@qi-duel/hand-right.h2r8", .source = qi_duel_right_rgba,
       .source_size = qi_duel_right_rgba_size},
      {.name = "@qi-duel/hud-states.h2r8", .source = qi_duel_hud_rgba,
       .source_size = qi_duel_hud_rgba_size},
      {.name = "@qi-duel/carousel-base.h2r8", .source = qi_duel_carousel_rgba,
       .source_size = qi_duel_carousel_rgba_size},
      {.name = "@qi-duel/charge-cells.h2r8", .source = qi_duel_charge_rgba,
       .source_size = qi_duel_charge_rgba_size},
      {.name = "@qi-duel/skill-styles.h2rs", .source = qi_duel_skill_styles,
       .source_size = qi_duel_skill_styles_size},
      {.name = "@qi-duel/skill-colors.h2rs", .source = qi_duel_skill_colors,
       .source_size = qi_duel_skill_colors_size},
#endif
  };
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
          /* Native fidelity pass retains the original-resolution opponent for
           * affine filtering. AMOLED memory/performance needs a separate audit. */
          .vm_memory_limit_bytes = 14u * 1024u * 1024u,
          .source_limit_bytes = 8u * 1024u * 1024u,
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
  qi_duel_link_t *link=h2_pal_mem_alloc(runtime->mem,sizeof(*link));
  if(link==NULL){h2_lua_host_destroy(host);return H2_PAL_ERR_NO_MEMORY;}
  memset(link,0,sizeof(*link));link->runtime=runtime;
  link->pause_management_advertising=config->pause_management_advertising;
  link->resume_management_advertising=config->resume_management_advertising;
  link->management_advertising_user=config->management_advertising_user;
  atomic_init(&link->connected,0);atomic_init(&link->central,0);
  h2_bloomspeaker_controller_init(&link->controller,0);
  result=h2_lua_register_module(host,"duel_link",qi_duel_link_open,link);
  if(result==H2_PAL_OK)result = h2_lua_host_start(host);
  if (result == H2_PAL_OK) {
    const h2_lua_arg_t args[] = {
        {.name = "layer", .value = config->layer ? config->layer : "full"},
        {.name = "time_ms", .value = config->time_ms ? config->time_ms : ""},
        {.name = "health_fx", .value = config->health_fx ? config->health_fx : ""},
        {.name = "charge_fx", .value = config->charge_fx ? config->charge_fx : ""},
        {.name = "qi", .value = config->qi ? config->qi : "3"},
        {.name = "click_controls", .value = config->click_controls ? "1" : "0"},
        {.name = "battle", .value = config->battle ? "1" : "0"},
        {.name = "draw_component", .value = config->draw_component ? config->draw_component : ""},
        {.name = "capture_step_ms", .value = config->capture_step_ms ? config->capture_step_ms : ""},
        {.name = "selected", .value = config->selected ? config->selected : "0"},
        {.name = "drag", .value = config->drag ? config->drag : ""},
        {.name = "action", .value = config->action ? config->action : ""},
        {.name = "actor", .value = config->actor ? config->actor : ""},
        {.name = "impact", .value = config->impact ? config->impact : ""},
        {.name = "clash", .value = config->clash ? config->clash : ""},
        {.name = "result", .value = config->result ? config->result : ""},
    };
    result = h2_lua_job_submit_resource(host, "@qi-duel/main.lua", args, sizeof(args)/sizeof(args[0]),
                                        &job_id);
  }
  while (result == H2_PAL_OK) {
    if (config->should_stop(config->should_stop_user)) {
      (void)h2_lua_job_cancel(host, job_id);
    }
    for (;;) {
      h2_pal_result_t poll_result = event_pending ? H2_PAL_OK :
          h2_runtime_poll_event(runtime, &event);
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
        if (result == H2_PAL_ERR_FULL) {
          /* A native draw can span multiple input events. Retain the current
           * payload and retry after the VM drains its bounded delivery queue;
           * do not discard button releases or terminate the application. */
          event_pending = 1;
          result = H2_PAL_OK;
          break;
        }
        event_pending = 0;
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
  int link_result=qi_duel_link_stop(link);
  if(link_result==H2_PAL_OK)h2_pal_mem_free(runtime->mem,link);
  else result=link_result; /* Keep callback context alive if task join failed. */
  return result;
}
