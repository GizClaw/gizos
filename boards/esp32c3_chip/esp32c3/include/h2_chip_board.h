#ifndef H2_CHIP_BOARD_H
#define H2_CHIP_BOARD_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

const char* h2_chip_board_name(void);
/** Fill a borrowed OS-only Runtime configuration. No physical peripherals.
 * Returns INVALID_ARG for NULL, otherwise OK. */
h2_pal_result_t h2_chip_board_runtime_config(h2_runtime_config_t* out_config);

#ifdef __cplusplus
}
#endif

#endif
