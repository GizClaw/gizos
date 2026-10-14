#pragma once
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t *out);
h2_pal_result_t h2_bk7258_board_runtime_deinit(void);
typedef void (*h2_bk7258_board_entry_task_fn)(void *user);
h2_pal_result_t h2_bk7258_board_start_entry_task(
    const char *name, h2_bk7258_board_entry_task_fn entry, void *user);
#ifdef __cplusplus
}
#endif
