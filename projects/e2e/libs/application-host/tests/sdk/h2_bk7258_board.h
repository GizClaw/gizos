#pragma once
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t *out);
h2_pal_result_t h2_bk7258_board_runtime_deinit(void);
#ifdef __cplusplus
}
#endif
