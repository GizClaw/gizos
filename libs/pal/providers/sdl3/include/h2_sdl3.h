#ifndef H2_SDL3_H
#define H2_SDL3_H

#include "h2/pal/hal/h2_pal_display.h"
#include "h2/pal/hal/h2_pal_touch.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_sdl3 h2_sdl3_t;

typedef struct h2_sdl3_config {
  const char *title;
  int width;
  int height;
} h2_sdl3_config_t;

typedef enum h2_sdl3_key {
  H2_SDL3_KEY_SPACE = 1,
  H2_SDL3_KEY_ENTER,
  H2_SDL3_KEY_ESCAPE,
  H2_SDL3_KEY_TAB,
  H2_SDL3_KEY_BACKSPACE,
  H2_SDL3_KEY_UP,
  H2_SDL3_KEY_DOWN,
  H2_SDL3_KEY_LEFT,
  H2_SDL3_KEY_RIGHT,
  H2_SDL3_KEY_A,
  H2_SDL3_KEY_B,
  H2_SDL3_KEY_C,
  H2_SDL3_KEY_D,
  H2_SDL3_KEY_E,
  H2_SDL3_KEY_F,
  H2_SDL3_KEY_G,
  H2_SDL3_KEY_H,
  H2_SDL3_KEY_I,
  H2_SDL3_KEY_J,
  H2_SDL3_KEY_K,
  H2_SDL3_KEY_L,
  H2_SDL3_KEY_M,
  H2_SDL3_KEY_N,
  H2_SDL3_KEY_O,
  H2_SDL3_KEY_P,
  H2_SDL3_KEY_Q,
  H2_SDL3_KEY_R,
  H2_SDL3_KEY_S,
  H2_SDL3_KEY_T,
  H2_SDL3_KEY_U,
  H2_SDL3_KEY_V,
  H2_SDL3_KEY_W,
  H2_SDL3_KEY_X,
  H2_SDL3_KEY_Y,
  H2_SDL3_KEY_Z,
  H2_SDL3_KEY_DIGIT_0,
  H2_SDL3_KEY_DIGIT_1,
  H2_SDL3_KEY_DIGIT_2,
  H2_SDL3_KEY_DIGIT_3,
  H2_SDL3_KEY_DIGIT_4,
  H2_SDL3_KEY_DIGIT_5,
  H2_SDL3_KEY_DIGIT_6,
  H2_SDL3_KEY_DIGIT_7,
  H2_SDL3_KEY_DIGIT_8,
  H2_SDL3_KEY_DIGIT_9,
  H2_SDL3_KEY_DELETE,
  H2_SDL3_KEY_PAGE_DOWN,
  H2_SDL3_KEY_PAGE_UP,
  H2_SDL3_KEY_HOME,
  H2_SDL3_KEY_END,
  H2_SDL3_KEY_KP_PLUS,
  H2_SDL3_KEY_KP_MINUS,
} h2_sdl3_key_t;

typedef enum h2_sdl3_event_kind {
  H2_SDL3_EVENT_CLOSE = 1,
  H2_SDL3_EVENT_FOCUS_LOST,
  H2_SDL3_EVENT_KEY,
  H2_SDL3_EVENT_TEXT,
  H2_SDL3_EVENT_WHEEL,
} h2_sdl3_event_kind_t;

typedef struct h2_sdl3_event {
  h2_sdl3_event_kind_t kind;
  h2_sdl3_key_t key;
  int pressed;
  int repeat;
  int32_t wheel_x;
  int32_t wheel_y;
  char text[32];
} h2_sdl3_event_t;

typedef struct h2_sdl3_pointer_state {
  int32_t x;
  int32_t y;
  int pressed;
} h2_sdl3_pointer_state_t;

int h2_sdl3_create(const h2_sdl3_config_t *config,
                   h2_sdl3_t **out_provider);
void h2_sdl3_destroy(h2_sdl3_t *provider);
h2_pal_display_t *h2_sdl3_display(h2_sdl3_t *provider);
const h2_pal_touch_api_t *h2_sdl3_touch(h2_sdl3_t *provider);
h2_pal_result_t h2_sdl3_poll_event(h2_sdl3_t *provider,
                                   h2_sdl3_event_t *out_event);
h2_pal_result_t h2_sdl3_read_pointer(h2_sdl3_t *provider,
                                     h2_sdl3_pointer_state_t *out_state);
void h2_sdl3_set_window_title(h2_sdl3_t *provider, const char *title);

#ifdef __cplusplus
}
#endif

#endif
