#include "h2_web_tap_reset_app.h"

#include "h2_tap_reset.h"

typedef struct h2_web_tap_reset_context {
  const h2_web_tap_reset_app_config_t *web;
} h2_web_tap_reset_context_t;

static h2_pal_result_t
web_tap_reset_read_pointer(void *user,
                           h2_tap_reset_pointer_state_t *out_state) {
  h2_web_tap_reset_context_t *context = user;
  if (context == NULL || context->web == NULL || out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return context->web->read_pointer(context->web->pointer_user, &out_state->x,
                                    &out_state->y, &out_state->pressed);
}

h2_pal_result_t
h2_web_tap_reset_app_run(h2_runtime_t *runtime,
                         const h2_web_tap_reset_app_config_t *config) {
  if (runtime == NULL || config == NULL || config->read_pointer == NULL ||
      config->should_stop == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_web_tap_reset_context_t context = {
      .web = config,
  };
  const h2_tap_reset_config_t tap_reset = {
      .eyebrow = "FIRMWARES / WASM HOST",
      .body = "LVGL UI\nRuntime + PAL boundary\nCanvas display and pointer",
      .increment_label = "Tap from WebAssembly",
      .read_pointer = web_tap_reset_read_pointer,
      .pointer_user = &context,
      .should_stop = config->should_stop,
      .stop_user = config->stop_user,
  };
  return h2_tap_reset_run(runtime, &tap_reset);
}
