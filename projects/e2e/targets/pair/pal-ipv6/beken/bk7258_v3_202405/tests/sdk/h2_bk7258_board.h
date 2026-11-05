#ifndef H2_IPV6_LAUNCHER_TEST_BOARD_H
#define H2_IPV6_LAUNCHER_TEST_BOARD_H
#include "h2_runtime.h"
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* config);
h2_pal_result_t h2_bk7258_board_start_entry_task(const char* name,
                                                 void (*entry)(void*),
                                                 void* user);
#endif
