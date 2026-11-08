#ifndef H2_ZERO_BK_PERIPHERAL_POWER_H
#define H2_ZERO_BK_PERIPHERAL_POWER_H

#include "core/h2_pal_errors.h"

/* The shared peripheral rail supplies mounted QSPI storage. Consumers may
 * enable it, but their open/close lifecycle must never release its output. */
h2_pal_result_t h2_zero_bk_peripheral_power_enable(void);
h2_pal_result_t h2_bk7258_board_audio_power_set(int enabled);

#endif
