#ifndef H2_SDL3_INTERNAL_H
#define H2_SDL3_INTERNAL_H

#include "h2_sdl3.h"

#include <SDL3/SDL.h>

#include <array>
#include <mutex>
#include <string>

enum {
  H2_SDL3_EVENT_QUEUE_CAPACITY = 64,
  H2_SDL3_TOUCH_QUEUE_CAPACITY = 64,
};

struct h2_sdl3 {
  SDL_Window *window = nullptr;
  SDL_Renderer *renderer = nullptr;
  SDL_Texture *texture = nullptr;
  uint16_t *framebuffer = nullptr;
  bool active = false;
  bool init_attempted = false;
  int init_result = H2_DISPLAY_ERR_UNAVAILABLE;
  bool initialized = false;
  size_t open_count = 0u;
  bool close_pending = false;
  bool present_pending = false;
  uint8_t brightness_mod = 255u;
  int width = 0;
  int height = 0;
  std::string title;
  std::mutex display_mutex;
  std::mutex event_mutex;
  h2_sdl3_pointer_state_t pointer = {};
  std::array<h2_sdl3_event_t, H2_SDL3_EVENT_QUEUE_CAPACITY> events = {};
  size_t event_head = 0u;
  size_t event_count = 0u;
  std::array<h2_pal_touch_event_t, H2_SDL3_TOUCH_QUEUE_CAPACITY> touches = {};
  size_t touch_head = 0u;
  size_t touch_count = 0u;
  size_t touch_open_count = 0u;
  h2_pal_display_t display = {};
  h2_pal_touch_api_t touch = {};
};

void h2_sdl3_enqueue_event(h2_sdl3_t *provider,
                           const h2_sdl3_event_t &event);
void h2_sdl3_enqueue_touch(h2_sdl3_t *provider,
                           const h2_pal_touch_event_t &event);
void h2_sdl3_init_touch(h2_sdl3_t *provider);
void h2_sdl3_pump(h2_sdl3_t *provider);
void h2_sdl3_cleanup_display(h2_sdl3_t *provider);
int h2_sdl3_present(h2_sdl3_t *provider);

#endif
