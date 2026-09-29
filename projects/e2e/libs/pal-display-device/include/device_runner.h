#ifndef H2_DISPLAY_DEVICE_RUNNER_H
#define H2_DISPLAY_DEVICE_RUNNER_H
#include "h2_pal_display_e2e.h"
int h2_display_device_run(h2_runtime_t *runtime, const char *version,
                          h2_pal_display_e2e_observe_fn observe, void *user,
                          const h2_pal_mem_api_t *working_mem);
/* One bounded dimming cycle, followed by close. */
int h2_display_device_demo(h2_runtime_t *runtime);
/* Leave the fixed pattern open at 100% for explicit physical inspection. */
int h2_display_device_show_pattern(h2_runtime_t *runtime);
void h2_display_device_replay(h2_runtime_t *runtime);
#endif
