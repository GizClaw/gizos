#ifndef H2_WEB_TAP_RESET_APP_H
#define H2_WEB_TAP_RESET_APP_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_WEB_TAP_RESET_WIDTH 360
#define H2_WEB_TAP_RESET_HEIGHT 640

typedef h2_pal_result_t (*h2_web_tap_reset_read_pointer_fn)(void *user,
                                                            int32_t *out_x,
                                                            int32_t *out_y,
                                                            int *out_pressed);

typedef struct h2_web_tap_reset_app_config {
  h2_web_tap_reset_read_pointer_fn read_pointer;
  void *pointer_user;
  int (*should_stop)(void *user);
  void *stop_user;
} h2_web_tap_reset_app_config_t;

h2_pal_result_t
h2_web_tap_reset_app_run(h2_runtime_t *runtime,
                         const h2_web_tap_reset_app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
