#ifndef H2_MOBILE_APP_H
#define H2_MOBILE_APP_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_MOBILE_APP_WIDTH 360
#define H2_MOBILE_APP_HEIGHT 640

typedef enum h2_mobile_platform {
  H2_MOBILE_PLATFORM_IOS = 0,
  H2_MOBILE_PLATFORM_ANDROID,
} h2_mobile_platform_t;

typedef struct h2_mobile_pointer_state {
  int32_t x;
  int32_t y;
  int pressed;
} h2_mobile_pointer_state_t;

typedef h2_pal_result_t (*h2_mobile_read_pointer_fn)(
    void *user, h2_mobile_pointer_state_t *out_state);

typedef struct h2_mobile_app_config {
  h2_mobile_platform_t platform;
  h2_mobile_read_pointer_fn read_pointer;
  void *pointer_user;
  int (*should_stop)(void *user);
  void *stop_user;
} h2_mobile_app_config_t;

/** Run the mobile-selected portable App until should_stop returns nonzero. */
h2_pal_result_t h2_mobile_app_run(h2_runtime_t *runtime,
                                  const h2_mobile_app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
