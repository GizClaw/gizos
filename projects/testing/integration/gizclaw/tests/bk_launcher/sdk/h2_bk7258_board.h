#ifndef H2_GIZCLAW_LAUNCHER_TEST_BOARD_H
#define H2_GIZCLAW_LAUNCHER_TEST_BOARD_H
#include "h2_runtime.h"
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* config);
void h2_bk7258_board_runtime_deinit(void);
h2_pal_mem_api_t* h2_bk7258_board_psram_allocator(void);
h2_pal_result_t h2_bk7258_board_start_entry_task(const char* name,
                                                 void (*entry)(void*),
                                                 void* user);
#endif
