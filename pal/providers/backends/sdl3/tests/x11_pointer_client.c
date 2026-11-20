/* The driver supplies real mouse edges from a second X11 client. */
#include "h2_sdl3.h"

#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void request_input(const char* stage) {
  puts(stage);
  fflush(stdout);
  assert(getchar() == '\n');
}

static unsigned pump(h2_sdl3_t* provider) {
  unsigned keys = 0;
  h2_sdl3_event_t event;
  while (h2_sdl3_poll_event(provider, &event) == H2_PAL_OK) {
    if (event.kind == H2_SDL3_EVENT_KEY && event.key == H2_SDL3_KEY_A &&
        event.pressed)
      ++keys;
  }
  return keys;
}

int main(void) {
  const h2_sdl3_config_t config = {
      .title = "X11 pointer", .width = 320, .height = 240};
  h2_sdl3_t* provider = NULL;
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  assert(SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1"));
  assert(h2_sdl3_create(&config, &provider) == H2_PAL_OK);
  h2_pal_display_t* display = h2_sdl3_display(provider);
  assert(h2_pal_display_open(display) == H2_PAL_OK);
  assert(strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0);
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  assert(windows && count == 1);
  SDL_WarpMouseInWindow(windows[0], 100, 90);
  SDL_free(windows);
  (void)pump(provider);

  bool owned = false;
  const h2_pal_touch_api_t* touch = h2_sdl3_touch(provider);
  assert(h2_pal_touch_open_pointer(touch, &owned) == H2_PAL_OK && owned);
  assert(h2_pal_touch_close_pointer(touch) == H2_PAL_OK);
  SDL_Event key = {0};
  key.type = SDL_EVENT_KEY_DOWN;
  key.key.scancode = SDL_SCANCODE_A;
  assert(SDL_PushEvent(&key));
  request_input("old clicks and held press");
  assert(h2_pal_touch_open_pointer(touch, &owned) == H2_PAL_OK && owned);
  assert(pump(provider) == 1);
  h2_pal_touch_sample_t sample;
  assert(h2_pal_touch_poll_pointer(touch, &sample) == H2_PAL_ERR_WOULD_BLOCK);

  request_input("release");
  (void)pump(provider);
  assert(h2_pal_touch_poll_pointer(touch, &sample) == H2_PAL_OK &&
         sample.event.kind == H2_PAL_TOUCH_EVENT_UP && !sample.down_after_open);
  assert(h2_pal_touch_poll_pointer(touch, &sample) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_touch_close_pointer(touch) == H2_PAL_OK);
  assert(h2_pal_touch_open_pointer(touch, &owned) == H2_PAL_OK && owned);
  request_input("first click after idle restart");
  (void)pump(provider);
  assert(h2_pal_touch_poll_pointer(touch, &sample) == H2_PAL_OK &&
         sample.event.kind == H2_PAL_TOUCH_EVENT_DOWN && sample.fresh_down &&
         sample.down_after_open);
  assert(h2_pal_touch_poll_pointer(touch, &sample) == H2_PAL_OK &&
         sample.event.kind == H2_PAL_TOUCH_EVENT_UP && !sample.down_after_open);
  assert(h2_pal_touch_poll_pointer(touch, &sample) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_touch_close_pointer(touch) == H2_PAL_OK);
  assert(h2_pal_display_close(display) == H2_PAL_OK);
  h2_sdl3_destroy(provider);
  puts("PASS X11 stale presses, retained key, release and first fresh click");
  return 0;
}
