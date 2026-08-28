#include "include/lvgl/lvgl.h"

LV_FONT_DECLARE(downstream_font_16);

int downstream_font_line_height(void) {
  return downstream_font_16.line_height;
}
