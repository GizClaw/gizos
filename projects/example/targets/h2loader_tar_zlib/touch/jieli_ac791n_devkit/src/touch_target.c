#include "h2_jieli_ac791n_devkit.h"
#include "h2_runtime.h"
#include "h2_touch_smoke.h"

#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct touch_target_state {
  h2_runtime_t *runtime;
  h2_pal_task_t *task;
  atomic_bool started;
  atomic_int startup_result;
} touch_target_state_t;

static touch_target_state_t touch_target;

static void emit(const char *format, ...) {
  char line[320];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (length > 0 && (size_t)length < sizeof(line)) {
    (void)h2_jieli_ac791n_devkit_console_write(
        line, (size_t)length, 100u);
  }
}

static const h2_pal_periph_single_button_payload_t action_button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

static const h2_pal_periph_info_t action_button = {
    .id = H2_JIELI_AC791N_ADKEY_OK_ID,
    .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
    .name = "ok",
    .payload = &action_button_payload,
    .payload_size = sizeof(action_button_payload),
};

static h2_pal_result_t periph_list(void *user, h2_pal_periph_type_t filter,
                                  h2_pal_periph_cb_t callback,
                                  void *callback_user) {
  (void)user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON) {
    return H2_PAL_OK;
  }
  return callback(callback_user, &action_button);
}

static h2_pal_result_t periph_get(void *user, h2_pal_periph_id_t id,
                                 h2_pal_periph_info_t *out_info) {
  (void)user;
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (id != action_button.id) return H2_PAL_ERR_NOT_FOUND;
  *out_info = action_button;
  return H2_PAL_OK;
}

static const h2_pal_periph_vtable_t periph_vtable = {
    .list = periph_list,
    .get = periph_get,
};

static const h2_pal_periph_api_t periph_api = {
    .user = NULL,
    .vtable = &periph_vtable,
};

static h2_pal_result_t mapper_list(
    void *user, h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t callback, void *callback_user) {
  (void)user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON) {
    return H2_PAL_OK;
  }
  const h2_runtime_component_mapping_entry_t entry = {
      .component_id = H2_TOUCH_SMOKE_COMPONENT_ACTION_BUTTON,
      .periph_id = H2_JIELI_AC791N_ADKEY_OK_ID,
  };
  return callback(callback_user, &entry);
}

static h2_pal_result_t mapper_get(
    void *user, h2_runtime_component_id_t component_id,
    h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (component_id != H2_TOUCH_SMOKE_COMPONENT_ACTION_BUTTON) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_periph_id = H2_JIELI_AC791N_ADKEY_OK_ID;
  return H2_PAL_OK;
}

static const h2_runtime_component_mapper_vtable_t mapper_vtable = {
    .list = mapper_list,
    .get_periph_id = mapper_get,
};

static const h2_runtime_component_mapper_t component_mapper = {
    .user = NULL,
    .vtable = &mapper_vtable,
};

static int should_stop(void *user) {
  (void)user;
  return 0;
}

static void on_started(void *user, h2_pal_result_t result) {
  touch_target_state_t *state = user;
  atomic_store_explicit(&state->startup_result, result, memory_order_release);
  atomic_store_explicit(&state->started, true, memory_order_release);
}

static void touch_task(void *user) {
  touch_target_state_t *state = user;
  static const h2_touch_smoke_config_t config = {
      .width = 480u,
      .height = 320u,
      .should_stop = should_stop,
      .stop_user = NULL,
      .on_started = on_started,
      .started_user = &touch_target,
  };
  int result = h2_touch_smoke_run(state->runtime, &config);
  emit("H2_JIELI_TOUCH_SMOKE stage=stopped result=%d\r\n", result);
}

int h2_jieli_target_application_run(void) {
  h2_runtime_config_t config;
  h2_runtime_t *runtime = NULL;
  emit("H2_JIELI_TOUCH_SMOKE stage=target-enter\r\n");
  memset(&touch_target, 0, sizeof(touch_target));
  atomic_init(&touch_target.started, false);
  atomic_init(&touch_target.startup_result, H2_PAL_ERR_INVALID_STATE);

  emit("H2_JIELI_TOUCH_SMOKE stage=runtime-config-enter\r\n");
  int result = h2_jieli_ac791n_devkit_runtime_config(&config);
  emit("H2_JIELI_TOUCH_SMOKE stage=runtime-config result=%d\r\n", result);
  if (result == H2_PAL_OK) {
    config.periph = &periph_api;
    config.component_mapper = &component_mapper;
    result = h2_runtime_init(&config, &runtime);
  }
  emit("H2_JIELI_TOUCH_SMOKE stage=runtime-init result=%d\r\n", result);
  if (result == H2_PAL_OK) result = h2_runtime_input_start(runtime, NULL);
  emit("H2_JIELI_TOUCH_SMOKE stage=input-start result=%d\r\n", result);
  if (result != H2_PAL_OK) return result;

  touch_target.runtime = runtime;
  const h2_pal_task_options_t options = {
      .name = "touch-smoke", .min_stack_size = 16384u};
  result = h2_pal_task_start(
      runtime->task, &options, touch_task, &touch_target, &touch_target.task);
  if (result != H2_PAL_OK) return result;

  for (unsigned attempt = 0u; attempt < 300u; ++attempt) {
    if (atomic_load_explicit(&touch_target.started, memory_order_acquire)) {
      result = atomic_load_explicit(
          &touch_target.startup_result, memory_order_acquire);
      emit("H2_JIELI_TOUCH_SMOKE_READY touch=ft6236 display=480x320 result=%d\r\n",
           result);
      return result;
    }
    (void)h2_pal_time_sleep_ms(runtime->time, 10u);
  }
  emit("H2_JIELI_TOUCH_SMOKE stage=start-timeout result=%d\r\n",
       H2_PAL_ERR_TIMEOUT);
  return H2_PAL_ERR_TIMEOUT;
}
