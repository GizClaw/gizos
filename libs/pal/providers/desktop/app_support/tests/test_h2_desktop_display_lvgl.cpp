#include "h2_desktop_app_support.h"
#include "h2_lvgl_platform.h"
#include "lvgl.h"

#include <SDL3/SDL.h>

#include <cassert>
#include <cstdlib>

namespace {

void run_lifecycle() {
  const h2_lvgl_platform_config_t platform = {
      h2_desktop_platform_default_allocator(), h2_desktop_platform_task_api(),
      h2_desktop_platform_sync_api(),          h2_desktop_platform_queue_api(),
      h2_desktop_platform_time_api(),
  };
  assert(h2_lvgl_platform_init(&platform) == H2_PAL_OK);
  lv_init();
  {
    const h2::desktop::Layout layout = {
        "display-lvgl-test", "Display/LVGL Test", 64, 48,
        nullptr,             0u,                  nullptr,
        0u,                  "{}",
    };
    h2::desktop::OwnedDisplay display;
    assert(h2::desktop::open_display(layout, &display) == H2_DISPLAY_OK);
    h2::desktop::OwnedLvgl lvgl;
    assert(h2::desktop::open_lvgl(&display, &lvgl) == H2_PAL_OK);
    assert(lvgl.display() != nullptr);
    assert(lvgl.keyboard() != nullptr);
    assert(lvgl.wheel() != nullptr);
    h2::desktop::OwnedStatusSurface status;
    assert(h2::desktop::open_status_surface(&display, "Status Test", &status) ==
           H2_DISPLAY_ERR_INVALID_STATE);

    int window_count = 0;
    SDL_Window **windows = SDL_GetWindows(&window_count);
    assert(windows != nullptr && window_count == 1);
    SDL_free(windows);

    assert(h2_pal_display_open(display.display()) == H2_DISPLAY_OK);
    windows = SDL_GetWindows(&window_count);
    assert(windows != nullptr && window_count == 1);
    SDL_free(windows);
    assert(h2_pal_display_close(display.display()) == H2_DISPLAY_OK);

    SDL_Event event = {};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 7.0f;
    event.button.y = 9.0f;
    assert(SDL_PushEvent(&event));
    assert(h2::desktop::poll_events(&display, &lvgl) == 0);
    h2_sdl3_pointer_state_t pointer = {};
    assert(h2::desktop::read_pointer(&display, &pointer) == H2_PAL_OK);
    assert(pointer.x == 7 && pointer.y == 9 && pointer.pressed == 1);

    event = {};
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&event));
    assert(h2::desktop::poll_events(&display, &lvgl) == 0);
    assert(h2::desktop::read_pointer(&display, &pointer) == H2_PAL_OK);
    assert(pointer.pressed == 0);

    event = {};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_RIGHT;
    assert(SDL_PushEvent(&event));
    event = {};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = "\xe4\xb8\xad";
    assert(SDL_PushEvent(&event));
    event = {};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = 1.0f;
    assert(SDL_PushEvent(&event));
    assert(h2::desktop::poll_events(&display, &lvgl) == 0);
    assert(lv_indev_get_key(static_cast<lv_indev_t *>(lvgl.keyboard())) ==
           0x4e2du);

    event = {};
    event.type = SDL_EVENT_QUIT;
    assert(SDL_PushEvent(&event));
    assert(h2::desktop::poll_events(&display, &lvgl) == 1);
  }
  lv_deinit();
  h2_lvgl_platform_deinit();
}

void run_status_lifecycle() {
  const h2::desktop::Layout layout = {
      "status-test", "Status Test", 320, 160, nullptr,
      0u,            nullptr,       0u,  "{}",
  };
  h2::desktop::OwnedDisplay display;
  h2::desktop::OwnedFilesystem filesystem;
  h2::desktop::Layout invalid_layout = layout;
  invalid_layout.mount_count = 1u;
  assert(h2::desktop::open_filesystem(invalid_layout, &filesystem) ==
         H2_PAL_FS_ERR_INVALID_ARG);
  assert(h2::desktop::open_display(layout, &display) == H2_DISPLAY_OK);
  {
    h2::desktop::OwnedStatusSurface status;
    assert(h2::desktop::open_status_surface(&display, "Status Test", &status) ==
           H2_DISPLAY_OK);
    int window_count = 0;
    SDL_Window **windows = SDL_GetWindows(&window_count);
    assert(windows != nullptr && window_count == 1);
    SDL_free(windows);
    assert(h2::desktop::set_process_state(
               &status, h2::desktop::ProcessState::kStopping) == H2_DISPLAY_OK);
    assert(h2::desktop::set_process_state(
               &status, h2::desktop::ProcessState::kFailed) == H2_DISPLAY_OK);
    assert(h2::desktop::set_process_state(
               &status, static_cast<h2::desktop::ProcessState>(0)) ==
           H2_DISPLAY_ERR_INVALID_ARG);
  }
  assert(!lv_is_initialized());
}

} // namespace

int main() {
  assert(setenv("SDL_VIDEODRIVER", "dummy", 1) == 0);
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  run_lifecycle();
  run_lifecycle();
  run_status_lifecycle();
  run_status_lifecycle();
  return 0;
}
