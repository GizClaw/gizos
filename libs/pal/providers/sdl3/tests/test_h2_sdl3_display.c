#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_sdl3.h"

#include <SDL3/SDL.h>

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void drain_host_events(h2_sdl3_t *provider) {
  h2_sdl3_event_t event;
  while (h2_sdl3_poll_event(provider, &event) == H2_PAL_OK) {
  }
}

static void test_lifecycle_and_events(void) {
  const h2_sdl3_config_t config = {
      .title = "SDL3 Provider Test",
      .width = 32,
      .height = 24,
  };
  h2_sdl3_t *provider = NULL;
  assert(h2_sdl3_create(&config, &provider) == H2_DISPLAY_OK);
  assert(provider != NULL);
  h2_sdl3_t *second = NULL;
  assert(h2_sdl3_create(&config, &second) == H2_PAL_ERR_BUSY);
  assert(second == NULL);
  h2_pal_display_t *display = h2_sdl3_display(provider);
  assert(h2_pal_display_open(display) == H2_DISPLAY_OK);
  h2_display_info_t info = {0};
  assert(h2_pal_display_get_info(display, &info) == H2_DISPLAY_OK);
  assert(info.width == 32 && info.height == 24);

  uint16_t pixels[8] = {0xffffu, 0xf800u, 0x07e0u, 0x001fu,
                        0u,      1u,      2u,      3u};
  const h2_display_rect_t rect = {.x = -1, .y = 0, .width = 4, .height = 2};
  assert(h2_pal_display_draw_bitmap(display, &rect, pixels,
                                    4u * sizeof(uint16_t),
                                    H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_OK);
  assert(h2_pal_display_present(display) == H2_DISPLAY_OK);
  drain_host_events(provider);

  const h2_pal_touch_api_t *touch = h2_sdl3_touch(provider);
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  h2_pal_touch_info_t touch_info = {0};
  assert(h2_pal_touch_get_info(touch, &touch_info) == H2_PAL_OK);
  assert(touch_info.width == 32u && touch_info.height == 24u);

  SDL_Event input = {0};
  input.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  input.button.button = SDL_BUTTON_LEFT;
  input.button.x = 7.0f;
  input.button.y = 9.0f;
  assert(SDL_PushEvent(&input));
  drain_host_events(provider);
  h2_pal_touch_event_t touch_event = {0};
  assert(h2_pal_touch_poll_event(touch, &touch_event) == H2_PAL_OK);
  assert(touch_event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(touch_event.x == 7 && touch_event.y == 9);

  input = (SDL_Event){0};
  input.type = SDL_EVENT_KEY_DOWN;
  input.key.scancode = SDL_SCANCODE_A;
  assert(SDL_PushEvent(&input));
  h2_sdl3_event_t projected = {0};
  assert(h2_sdl3_poll_event(provider, &projected) == H2_PAL_OK);
  assert(projected.kind == H2_SDL3_EVENT_KEY);
  assert(projected.key == H2_SDL3_KEY_A && projected.pressed == 1);

  assert(h2_pal_touch_close(touch) == H2_PAL_OK);
  assert(h2_pal_display_close(display) == H2_DISPLAY_OK);
  assert(h2_sdl3_poll_event(provider, &projected) == H2_PAL_OK);
  assert(projected.kind == H2_SDL3_EVENT_CLOSE);
  h2_sdl3_destroy(provider);
}

static void test_invalid_pitch(void) {
  const h2_sdl3_config_t config = {
      .title = "SDL3 Invalid Pitch Test",
      .width = INT_MAX,
      .height = 1,
  };
  h2_sdl3_t *provider = NULL;
  assert(h2_sdl3_create(&config, &provider) == H2_DISPLAY_ERR_INVALID_ARG);
  assert(provider == NULL);
}

int main(void) {
  assert(setenv("SDL_VIDEODRIVER", "dummy", 1) == 0);
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  test_invalid_pitch();
  test_lifecycle_and_events();
  test_lifecycle_and_events();
  return 0;
}
