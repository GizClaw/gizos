#include "h2_mobile_app.h"

#include "h2_tap_reset.h"

_Static_assert(H2_MOBILE_APP_WIDTH == H2_TAP_RESET_WIDTH,
               "mobile and tap-reset widths must match");
_Static_assert(H2_MOBILE_APP_HEIGHT == H2_TAP_RESET_HEIGHT,
               "mobile and tap-reset heights must match");

typedef struct h2_mobile_tap_reset_context {
  const h2_mobile_app_config_t *mobile;
} h2_mobile_tap_reset_context_t;

static h2_pal_result_t
mobile_tap_reset_read_pointer(void *user,
                              h2_tap_reset_pointer_state_t *out_state) {
  h2_mobile_tap_reset_context_t *context = user;
  if (context == NULL || context->mobile == NULL || out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_mobile_pointer_state_t pointer = {0};
  const h2_pal_result_t result =
      context->mobile->read_pointer(context->mobile->pointer_user, &pointer);
  if (result != H2_PAL_OK) {
    return result;
  }
  out_state->x = pointer.x;
  out_state->y = pointer.y;
  out_state->pressed = pointer.pressed;
  return H2_PAL_OK;
}

static h2_pal_result_t
mobile_tap_reset_presentation(h2_mobile_platform_t platform,
                              const char **out_eyebrow, const char **out_body,
                              const char **out_increment_label) {
  switch (platform) {
  case H2_MOBILE_PLATFORM_IOS:
    *out_eyebrow = "FIRMWARES / IOS HOST";
    *out_body = "LVGL UI\nRuntime + PAL boundary\nUIKit display and touch";
    *out_increment_label = "Tap from UIKit";
    return H2_PAL_OK;
  case H2_MOBILE_PLATFORM_ANDROID:
    *out_eyebrow = "FIRMWARES / ANDROID HOST";
    *out_body = "LVGL UI\nRuntime + PAL boundary\nAndroid bitmap and touch";
    *out_increment_label = "Tap from Android";
    return H2_PAL_OK;
  default:
    return H2_PAL_ERR_INVALID_ARG;
  }
}

h2_pal_result_t h2_mobile_app_run(h2_runtime_t *runtime,
                                  const h2_mobile_app_config_t *config) {
  if (runtime == NULL || config == NULL || config->read_pointer == NULL ||
      config->should_stop == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const char *eyebrow = NULL;
  const char *body = NULL;
  const char *increment_label = NULL;
  const h2_pal_result_t presentation_result = mobile_tap_reset_presentation(
      config->platform, &eyebrow, &body, &increment_label);
  if (presentation_result != H2_PAL_OK) {
    return presentation_result;
  }
  h2_mobile_tap_reset_context_t context = {
      .mobile = config,
  };
  const h2_tap_reset_config_t tap_reset = {
      .eyebrow = eyebrow,
      .body = body,
      .increment_label = increment_label,
      .read_pointer = mobile_tap_reset_read_pointer,
      .pointer_user = &context,
      .should_stop = config->should_stop,
      .stop_user = config->stop_user,
  };
  return h2_tap_reset_run(runtime, &tap_reset);
}
