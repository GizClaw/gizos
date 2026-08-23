#include "global_mouse_source.h"

#include <SDL3/SDL.h>

#import <AppKit/AppKit.h>

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
  auto *sdl_window = static_cast<SDL_Window *>(window_pointer);
  if (sdl_window == nullptr || out_x == nullptr || out_y == nullptr) {
    return false;
  }
  void *native_pointer = SDL_GetPointerProperty(
      SDL_GetWindowProperties(sdl_window),
      SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
      nullptr);
  NSWindow *window = (__bridge NSWindow *)native_pointer;
  NSView *content_view = window.contentView;
  if (window == nil || content_view == nil) {
    return false;
  }

  const NSPoint screen_point = NSEvent.mouseLocation;
  const NSPoint window_point = [window convertPointFromScreen:screen_point];
  const NSPoint content_point =
      [content_view convertPoint:window_point fromView:nil];
  const NSRect bounds = content_view.bounds;
  *out_x = static_cast<std::int32_t>(content_point.x);
  *out_y = static_cast<std::int32_t>(NSMaxY(bounds) - content_point.y);
  return true;
}

}  // namespace h2::starboy
