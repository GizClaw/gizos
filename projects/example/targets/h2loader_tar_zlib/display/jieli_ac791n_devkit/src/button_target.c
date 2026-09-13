#include "h2_button_smoke.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_runtime.h"

#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
  H2_BUTTON_COMPONENT_POWER = 1,
  H2_BUTTON_COMPONENT_ENCODER,
  H2_BUTTON_COMPONENT_PHOTO,
  H2_BUTTON_COMPONENT_OK,
  H2_BUTTON_COMPONENT_VOLUME_UP,
  H2_BUTTON_COMPONENT_VOLUME_DOWN,
  H2_BUTTON_COMPONENT_MODE,
  H2_BUTTON_COMPONENT_CANCEL,
};

typedef struct button_target_state {
  h2_runtime_t *runtime;
  h2_pal_task_t *task;
  atomic_bool started;
  atomic_int startup_result;
} button_target_state_t;

static button_target_state_t button_target;

static void emit(const char *format, ...) {
  char line[320];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (length > 0 && (size_t)length < sizeof(line)) {
    (void)h2_jieli_ac791n_devkit_console_write(line, (size_t)length, 100u);
  }
}

static const h2_pal_periph_single_button_payload_t poll_button = {
    .delivery = H2_PAL_BUTTON_DELIVERY_POLL_STATE,
};

#define BUTTON_INFO(periph, label)                                         \
  {                                                                        \
      .id = (periph), .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,           \
      .name = label, .payload = &poll_button,                              \
      .payload_size = sizeof(poll_button),                                 \
  }

static const h2_pal_periph_info_t button_periphs[] = {
    BUTTON_INFO(H2_JIELI_AC791N_ADKEY_POWER_ID, "power"),
    BUTTON_INFO(H2_JIELI_AC791N_ADKEY_ENCODER_ID, "encoder"),
    BUTTON_INFO(H2_JIELI_AC791N_ADKEY_PHOTO_ID, "photo"),
    BUTTON_INFO(H2_JIELI_AC791N_ADKEY_OK_ID, "ok"),
    BUTTON_INFO(H2_JIELI_AC791N_ADKEY_VOLUME_UP_ID, "volume_up"),
    BUTTON_INFO(H2_JIELI_AC791N_ADKEY_VOLUME_DOWN_ID, "volume_down"),
    BUTTON_INFO(H2_JIELI_AC791N_ADKEY_MODE_ID, "mode"),
    BUTTON_INFO(H2_JIELI_AC791N_ADKEY_CANCEL_ID, "cancel"),
};

static const h2_runtime_component_mapping_entry_t button_mappings[] = {
    {H2_BUTTON_COMPONENT_POWER, H2_JIELI_AC791N_ADKEY_POWER_ID},
    {H2_BUTTON_COMPONENT_ENCODER, H2_JIELI_AC791N_ADKEY_ENCODER_ID},
    {H2_BUTTON_COMPONENT_PHOTO, H2_JIELI_AC791N_ADKEY_PHOTO_ID},
    {H2_BUTTON_COMPONENT_OK, H2_JIELI_AC791N_ADKEY_OK_ID},
    {H2_BUTTON_COMPONENT_VOLUME_UP, H2_JIELI_AC791N_ADKEY_VOLUME_UP_ID},
    {H2_BUTTON_COMPONENT_VOLUME_DOWN,
     H2_JIELI_AC791N_ADKEY_VOLUME_DOWN_ID},
    {H2_BUTTON_COMPONENT_MODE, H2_JIELI_AC791N_ADKEY_MODE_ID},
    {H2_BUTTON_COMPONENT_CANCEL, H2_JIELI_AC791N_ADKEY_CANCEL_ID},
};

static const h2_button_smoke_button_t app_buttons[] = {
    {H2_BUTTON_COMPONENT_POWER, "POWER"},
    {H2_BUTTON_COMPONENT_ENCODER, "ENCODER"},
    {H2_BUTTON_COMPONENT_PHOTO, "PHOTO"},
    {H2_BUTTON_COMPONENT_OK, "OK"},
    {H2_BUTTON_COMPONENT_VOLUME_UP, "VOL+"},
    {H2_BUTTON_COMPONENT_VOLUME_DOWN, "VOL-"},
    {H2_BUTTON_COMPONENT_MODE, "MODE"},
    {H2_BUTTON_COMPONENT_CANCEL, "CANCEL"},
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
  for (size_t i = 0u; i < sizeof(button_periphs) / sizeof(button_periphs[0]);
       ++i) {
    h2_pal_result_t result = callback(callback_user, &button_periphs[i]);
    if (result != H2_PAL_OK) return result;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t periph_get(void *user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t *out_info) {
  (void)user;
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < sizeof(button_periphs) / sizeof(button_periphs[0]);
       ++i) {
    if (button_periphs[i].id == id) {
      *out_info = button_periphs[i];
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
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
  for (size_t i = 0u; i < sizeof(button_mappings) / sizeof(button_mappings[0]);
       ++i) {
    h2_pal_result_t result = callback(callback_user, &button_mappings[i]);
    if (result != H2_PAL_OK) return result;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t mapper_get(
    void *user, h2_runtime_component_id_t component_id,
    h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL) return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < sizeof(button_mappings) / sizeof(button_mappings[0]);
       ++i) {
    if (button_mappings[i].component_id == component_id) {
      *out_periph_id = button_mappings[i].periph_id;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
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
  button_target_state_t *state = user;
  atomic_store_explicit(&state->startup_result, result, memory_order_release);
  atomic_store_explicit(&state->started, true, memory_order_release);
}

static void button_task(void *user) {
  button_target_state_t *state = user;
  static const h2_button_smoke_config_t config = {
      .width = 480u,
      .height = 320u,
      .buttons = app_buttons,
      .button_count = sizeof(app_buttons) / sizeof(app_buttons[0]),
      .should_stop = should_stop,
      .on_started = on_started,
      .started_user = &button_target,
  };
  int result = h2_button_smoke_run(state->runtime, &config);
  emit("H2_JIELI_BUTTON_SMOKE stage=stopped result=%d\r\n", result);
}

int h2_jieli_target_application_run(void) {
  h2_runtime_config_t config;
  h2_runtime_t *runtime = NULL;
  emit("H2_JIELI_BUTTON_SMOKE stage=target-enter\r\n");
  memset(&button_target, 0, sizeof(button_target));
  atomic_init(&button_target.started, false);
  atomic_init(&button_target.startup_result, H2_PAL_ERR_INVALID_STATE);
  int result = h2_jieli_ac791n_devkit_runtime_config(&config);
  if (result == H2_PAL_OK) {
    config.periph = &periph_api;
    config.component_mapper = &component_mapper;
    result = h2_runtime_init(&config, &runtime);
  }
  emit("H2_JIELI_BUTTON_SMOKE stage=runtime-init result=%d\r\n", result);
  if (result == H2_PAL_OK) result = h2_runtime_input_start(runtime, NULL);
  emit("H2_JIELI_BUTTON_SMOKE stage=input-start result=%d\r\n", result);
  if (result != H2_PAL_OK) return result;

  button_target.runtime = runtime;
  const h2_pal_task_options_t options = {
      .name = "button-smoke", .min_stack_size = 16384u};
  result = h2_pal_task_start(runtime->task, &options, button_task,
                             &button_target, &button_target.task);
  if (result != H2_PAL_OK) return result;
  for (unsigned attempt = 0u; attempt < 300u; ++attempt) {
    if (atomic_load_explicit(&button_target.started, memory_order_acquire)) {
      result = atomic_load_explicit(&button_target.startup_result,
                                    memory_order_acquire);
      emit("H2_JIELI_BUTTON_SMOKE_READY buttons=8 display=480x320 result=%d\r\n",
           result);
      return result;
    }
    (void)h2_pal_time_sleep_ms(runtime->time, 10u);
  }
  return H2_PAL_ERR_TIMEOUT;
}
