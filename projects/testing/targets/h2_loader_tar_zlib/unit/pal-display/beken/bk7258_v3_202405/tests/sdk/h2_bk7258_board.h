#ifndef H2_DISPLAY_LAUNCHER_TEST_BOARD_H
#define H2_DISPLAY_LAUNCHER_TEST_BOARD_H
#include "h2_runtime.h"

h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* config);
h2_pal_result_t h2_bk7258_board_start_entry_task(const char* name,
                                                 void (*entry)(void*),
                                                 void* user);
int h2_bk7258_board_display_capture(uint16_t* out, size_t count);

#ifdef H2_TEST_DISPLAY_TRANSFER
typedef void (*test_transfer_fn)(void*, const h2_display_rect_t*,
                                 const uint8_t*, size_t);
h2_display_info_t test_board_transfer_capture(test_transfer_fn callback,
                                              void* user);
uint32_t test_board_backlight_percent(void);
#define H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE(callback, user) \
  test_board_transfer_capture(callback, user)
#define H2_BK7258_BOARD_DISPLAY_BACKLIGHT_PERCENT() \
  test_board_backlight_percent()
#endif
#endif
