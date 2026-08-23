#ifndef H2_STARBOY_AMOLED_BOARD_CONFIG_H
#define H2_STARBOY_AMOLED_BOARD_CONFIG_H

#include "h2_runtime.h"
#include "h2_starboy.h"

#ifdef __cplusplus
extern "C" {
#endif

h2_pal_result_t h2_starboy_amoled_runtime_config(
    h2_runtime_config_t *out_config);

/* Board-owned poll cadence and task policy for Runtime input acquisition. */
h2_pal_result_t h2_starboy_amoled_input_poll_config(
    h2_runtime_input_poll_config_t *out_config);

h2_starboy_motion_api_t h2_starboy_amoled_motion_api(
    h2_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
