#ifndef H2_LVGL_SMOKE_H
#define H2_LVGL_SMOKE_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef h2_pal_result_t (*h2_lvgl_smoke_ready_fn)(void *user);

typedef struct h2_lvgl_smoke_config {
  /** Allocator for the LVGL heap and full-screen render buffer. */
  const h2_pal_mem_api_t *mem;
  /** Opaque user pointer passed to `ready`. */
  void *ready_user;
  /** Called once after the first complete frame is presented. */
  h2_lvgl_smoke_ready_fn ready;
} h2_lvgl_smoke_config_t;

/**
 * @brief Run the blocking LVGL smoke screen.
 *
 * The App uses one full-screen RGB565 render buffer with
 * `LV_DISPLAY_RENDER_MODE_FULL`. The call only returns on initialization,
 * rendering, callback, or Runtime capability failure.
 */
h2_pal_result_t h2_lvgl_smoke_run(h2_runtime_t *runtime,
                                  const h2_lvgl_smoke_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
