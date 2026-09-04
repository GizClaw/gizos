#ifndef H2_BLOOMSPEAKER_AMOLED_BOARD_CONFIG_H
#define H2_BLOOMSPEAKER_AMOLED_BOARD_CONFIG_H

#include "h2_runtime.h"

h2_pal_result_t h2_bloomspeaker_amoled_runtime_config(
    h2_runtime_config_t *out_config);

h2_pal_result_t h2_bloomspeaker_amoled_input_poll_config(
    h2_runtime_input_poll_config_t *out_config);

#endif
