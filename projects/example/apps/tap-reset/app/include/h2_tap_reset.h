#ifndef H2_TAP_RESET_H
#define H2_TAP_RESET_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_TAP_RESET_WIDTH 360
#define H2_TAP_RESET_HEIGHT 640

typedef struct h2_tap_reset_pointer_state {
  int32_t x;
  int32_t y;
  int pressed;
} h2_tap_reset_pointer_state_t;

typedef h2_pal_result_t (*h2_tap_reset_read_pointer_fn)(
    void *user, h2_tap_reset_pointer_state_t *out_state);

typedef struct h2_tap_reset_config {
  const char *eyebrow;
  const char *body;
  const char *increment_label;
  h2_tap_reset_read_pointer_fn read_pointer;
  void *pointer_user;
  int (*should_stop)(void *user);
  void *stop_user;
} h2_tap_reset_config_t;

/**
 * Run the portable LVGL tap/reset smoke App until should_stop returns nonzero.
 *
 * The Runtime, callback contexts, and callback implementations are borrowed
 * for the duration of this blocking call. The caller owns platform lifecycle
 * and must invoke this entry on a thread dedicated to the portable App.
 */
h2_pal_result_t h2_tap_reset_run(h2_runtime_t *runtime,
                                 const h2_tap_reset_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
