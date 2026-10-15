#ifndef H2_TOUCH_SMOKE_H
#define H2_TOUCH_SMOKE_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_TOUCH_SMOKE_COMPONENT_ACTION_BUTTON 1u

typedef struct h2_touch_smoke_config {
    uint32_t width;
    uint32_t height;
    int (*should_stop)(void *user);
    void *stop_user;
    /** Optional startup handshake, invoked once after UI/touch initialization. */
    void (*on_started)(void *user, h2_pal_result_t result);
    void *started_user;
} h2_touch_smoke_config_t;

/** Run the LVGL Touch PAL and Runtime Button smoke flow. */
h2_pal_result_t h2_touch_smoke_run(
    h2_runtime_t *runtime,
    const h2_touch_smoke_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
