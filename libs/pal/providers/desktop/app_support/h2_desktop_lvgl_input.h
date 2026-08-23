#ifndef H2_DESKTOP_LVGL_INPUT_H
#define H2_DESKTOP_LVGL_INPUT_H

#include "h2_sdl3.h"

#include <lvgl.h>

struct H2DesktopLvglInput;

int h2_desktop_lvgl_input_create(lv_display_t *display,
                                 H2DesktopLvglInput **out_input);
void h2_desktop_lvgl_input_destroy(H2DesktopLvglInput *input);
void h2_desktop_lvgl_input_handle(H2DesktopLvglInput *input,
                                  const h2_sdl3_event_t &event);
lv_indev_t *h2_desktop_lvgl_input_keyboard(H2DesktopLvglInput *input);
lv_indev_t *h2_desktop_lvgl_input_wheel(H2DesktopLvglInput *input);

#endif
