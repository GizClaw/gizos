#include "os/h2_pal_time.h"
#include "h2_g99_e2e_internal.h"
#include "h2_runtime_custom_event.h"

#include <string.h>

#define H2_G99_E2E_MAIN_TASK "g99-e2e/main"
#define H2_G99_E2E_MAIN_STACK (256u * 1024u)

uint64_t h2_g99_e2e_now_ms(const h2_g99_e2e_t* e2e) {
  uint64_t now = 0u;
  (void)h2_pal_time_get_monotonic_ms(e2e->runtime->time, &now);
  return now;
}

void h2_g99_e2e_sleep(h2_g99_e2e_t* e2e, uint32_t ms) {
  (void)h2_pal_time_sleep_ms(e2e->runtime->time, ms);
}

void h2_g99_e2e_lock(h2_g99_e2e_t* e2e) {
  (void)h2_pal_mutex_lock(e2e->runtime->sync, e2e->mutex);
}

void h2_g99_e2e_unlock(h2_g99_e2e_t* e2e) {
  (void)h2_pal_mutex_unlock(e2e->runtime->sync, e2e->mutex);
}

static void lock(h2_g99_e2e_t* e2e) { h2_g99_e2e_lock(e2e); }

static void unlock(h2_g99_e2e_t* e2e) { h2_g99_e2e_unlock(e2e); }

static void on_observation(void* user, const h2_g99_observation_t* o) {
  h2_g99_e2e_t* e2e = user;
  const uint64_t now = h2_g99_e2e_now_ms(e2e);
  lock(e2e);
  e2e->observation = *o;
  e2e->observed_ms = now;
  e2e->observed = true;
  unlock(e2e);
  if (e2e->target->observe != NULL)
    e2e->target->observe(e2e->target->observe_user, o);
}

static uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

static void on_frame(void* user, const h2_g99_frame_stats_t* frame) {
  h2_g99_e2e_t* e2e = user;
  lock(e2e);
  h2_g99_e2e_frames_t* f = &e2e->frames;
  /* The first frame after a reset has no interval inside the window. */
  if (f->frames > 0u) {
    f->interval_us += frame->interval_us;
    f->max_interval_us = max_u32(f->max_interval_us, frame->interval_us);
  }
  ++f->frames;
  f->work_us += frame->work_us;
  f->max_work_us = max_u32(f->max_work_us, frame->work_us);
  f->max_draw_us = max_u32(f->max_draw_us, frame->draw_us);
  f->max_present_us = max_u32(f->max_present_us, frame->present_us);
  f->pixels += frame->pixels;
  unlock(e2e);
}

void h2_g99_e2e_frames_reset(h2_g99_e2e_t* e2e) {
  lock(e2e);
  memset(&e2e->frames, 0, sizeof(e2e->frames));
  unlock(e2e);
}

void h2_g99_e2e_frames_get(h2_g99_e2e_t* e2e, h2_g99_e2e_frames_t* out) {
  lock(e2e);
  *out = e2e->frames;
  unlock(e2e);
}

static int should_stop(void* user) {
  h2_g99_e2e_t* e2e = user;
  lock(e2e);
  const bool stop = e2e->stop_main;
  unlock(e2e);
  return stop ? 1 : 0;
}

static void main_task(void* user) {
  h2_g99_e2e_t* e2e = user;
  const h2_pal_result_t rc = h2_g99_run(e2e->runtime, &e2e->main_config);
  lock(e2e);
  e2e->main_result = rc;
  e2e->main_exited = true;
  unlock(e2e);
}

h2_pal_result_t h2_g99_e2e_session_open(h2_g99_e2e_t* e2e,
                                        const h2_g99_e2e_target_t* target) {
  memset(e2e, 0, sizeof(*e2e));
  e2e->target = target;
  e2e->runtime = target->runtime;
  e2e->main_config.main_version = "e2e";
  e2e->main_config.board_name = target->board;
  e2e->main_config.should_stop = should_stop;
  e2e->main_config.should_stop_user = e2e;
  e2e->main_config.observe = on_observation;
  e2e->main_config.observe_user = e2e;
  e2e->main_config.settings_namespace = H2_G99_E2E_SETTINGS_NAMESPACE;
  e2e->main_config.frame = on_frame;
  e2e->main_config.frame_user = e2e;
  e2e->main_config.render_mem = target->render_mem;
  /* The suite owns pointer input throughout automation. The real touch
   * device is opened for its capability contract, not fed into the cases. */
  h2_g99_e2e_finger_init(e2e, target->runtime->touch);
  e2e->main_config.touch = &e2e->finger.api;
  const h2_pal_mutex_config_t config = {"g99-e2e", e2e->runtime->mem, 0u};
  h2_pal_result_t rc =
      h2_pal_mutex_create(e2e->runtime->sync, &config, &e2e->mutex);
  if (rc == H2_PAL_OK)
    rc = h2_runtime_test_control_open(e2e->runtime, &e2e->control);
  return rc;
}

void h2_g99_e2e_session_close(h2_g99_e2e_t* e2e) {
  if (e2e->control != NULL) h2_runtime_test_control_close(e2e->control);
  e2e->control = NULL;
  if (e2e->mutex != NULL)
    (void)h2_pal_mutex_destroy(e2e->runtime->sync, e2e->mutex);
  e2e->mutex = NULL;
}

h2_pal_result_t h2_g99_e2e_main_start(h2_g99_e2e_t* e2e) {
  if (e2e->main_task != NULL) return H2_PAL_ERR_INVALID_STATE;
  lock(e2e);
  memset(&e2e->observation, 0, sizeof(e2e->observation));
  e2e->observed = false;
  e2e->main_exited = false;
  e2e->main_result = H2_PAL_OK;
  e2e->stop_main = false;
  unlock(e2e);
  const h2_pal_task_options_t options = {H2_G99_E2E_MAIN_TASK,
                                         H2_G99_E2E_MAIN_STACK};
  return h2_pal_task_start(e2e->runtime->task, &options, main_task, e2e,
                           &e2e->main_task);
}

h2_pal_result_t h2_g99_e2e_main_stop(h2_g99_e2e_t* e2e) {
  if (e2e->main_task == NULL) return H2_PAL_OK;
  lock(e2e);
  e2e->stop_main = true;
  unlock(e2e);
  const h2_pal_result_t joined =
      h2_pal_task_join(e2e->runtime->task, e2e->main_task);
  if (joined != H2_PAL_OK) return joined;
  e2e->main_task = NULL;
  lock(e2e);
  const h2_pal_result_t rc = e2e->main_result;
  unlock(e2e);
  return rc;
}

h2_pal_result_t h2_g99_e2e_main_restart(h2_g99_e2e_t* e2e) {
  const h2_pal_result_t stopped = h2_g99_e2e_main_stop(e2e);
  if (stopped != H2_PAL_OK) {
    h2_g99_e2e_errorf(e2e, stopped, "Main stop");
    return stopped;
  }
  const h2_pal_result_t started = h2_g99_e2e_main_start(e2e);
  if (started != H2_PAL_OK) h2_g99_e2e_errorf(e2e, started, "Main start");
  return started;
}

bool h2_g99_e2e_main_running(h2_g99_e2e_t* e2e) {
  if (e2e->main_task == NULL) return false;
  lock(e2e);
  const bool exited = e2e->main_exited;
  unlock(e2e);
  return !exited;
}

bool h2_g99_e2e_observe(h2_g99_e2e_t* e2e, h2_g99_observation_t* out,
                        uint64_t* out_observed_ms) {
  lock(e2e);
  const bool observed = e2e->observed;
  if (out != NULL) *out = e2e->observation;
  if (out_observed_ms != NULL) *out_observed_ms = e2e->observed_ms;
  unlock(e2e);
  return observed;
}

h2_pal_result_t h2_g99_e2e_press(h2_g99_e2e_t* e2e,
                                 h2_runtime_component_id_t component) {
  /* Main acts once per press timestamp, so every press gets a new one. */
  h2_runtime_timestamp_ms_t pressed_at = h2_g99_e2e_now_ms(e2e);
  if (pressed_at <= e2e->last_pressed_at)
    pressed_at = e2e->last_pressed_at + 1u;
  e2e->last_pressed_at = pressed_at;
  h2_pal_result_t rc =
      h2_runtime_test_button_down(e2e->control, component, pressed_at);
  if (rc == H2_PAL_OK) {
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_KEY_HOLD_MS);
    h2_runtime_timestamp_ms_t released_at = h2_g99_e2e_now_ms(e2e);
    if (released_at <= pressed_at) released_at = pressed_at + 1u;
    rc = h2_runtime_test_button_up(e2e->control, component, pressed_at,
                                   released_at);
  }
  if (rc != H2_PAL_OK) h2_g99_e2e_errorf(e2e, rc, "key injection");
  return rc;
}

h2_pal_result_t h2_g99_e2e_type(h2_g99_e2e_t* e2e, const char* text) {
  const h2_runtime_custom_event_t event = {H2_G99_EVENT_TEXT_INPUT, text,
                                           strlen(text)};
  const h2_pal_result_t rc = h2_runtime_post_custom_event(e2e->runtime, &event);
  if (rc != H2_PAL_OK) h2_g99_e2e_errorf(e2e, rc, "text input");
  return rc;
}

bool h2_g99_e2e_aborted(h2_g99_e2e_t* e2e) {
  return e2e->target->should_abort != NULL &&
         e2e->target->should_abort(e2e->target->abort_user) != 0;
}
