#include "global_mouse_source.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace h2::starboy {

void *find_global_mouse_window(const char *window_title) {
  if (window_title == nullptr) {
    return nullptr;
  }
  int window_count = 0;
  SDL_Window **windows = SDL_GetWindows(&window_count);
  if (windows == nullptr) {
    return nullptr;
  }
  SDL_Window *matched = nullptr;
  for (int index = 0; index < window_count; ++index) {
    const char *title = SDL_GetWindowTitle(windows[index]);
    if (title != nullptr && std::strcmp(title, window_title) == 0) {
      matched = windows[index];
      break;
    }
  }
  SDL_free(windows);
  return matched;
}

bool sample_global_mouse_relative(void *window_pointer,
                                  std::int32_t *out_x,
                                  std::int32_t *out_y) {
  auto *window = static_cast<SDL_Window *>(window_pointer);
  if (window == nullptr || out_x == nullptr || out_y == nullptr) {
    return false;
  }
  float global_x = 0.0F;
  float global_y = 0.0F;
  (void)SDL_GetGlobalMouseState(&global_x, &global_y);
  int window_x = 0;
  int window_y = 0;
  if (!SDL_GetWindowPosition(window, &window_x, &window_y)) {
    return false;
  }
  *out_x = static_cast<std::int32_t>(global_x) - window_x;
  *out_y = static_cast<std::int32_t>(global_y) - window_y;
  return true;
}

}  // namespace h2::starboy
