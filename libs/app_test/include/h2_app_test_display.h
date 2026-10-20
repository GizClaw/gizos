#ifndef H2_APP_TEST_DISPLAY_H
#define H2_APP_TEST_DISPLAY_H
#include "h2/pal/hal/h2_pal_display.h"
#include "h2_app_test_fault.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Headless Display PAL for lifecycle and brightness assertions. Drawing is
 * unsupported by default. accept_bitmaps validates RGB565 rectangle/stride
 * inputs and counts submissions without retaining pixels or opening a window;
 * consumers may observe their own renderer's frames for golden comparison. */
typedef struct h2_app_test_display {
  h2_pal_display_api_t api;
  h2_display_info_t info;
  bool opened;
  bool accept_bitmaps;
  size_t draw_count, present_count;
  uint32_t brightness_percent, last_brightness_percent;
  h2_app_test_fault_t open, close, brightness;
} h2_app_test_display_t;
/** Initialize caller storage; NULL is a no-op. Call PAL open before use. */
void h2_app_test_display_init(h2_app_test_display_t* display);

#ifdef __cplusplus
}
#endif
#endif
