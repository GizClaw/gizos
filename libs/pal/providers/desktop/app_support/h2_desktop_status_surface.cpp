#include "h2_desktop_app_support.h"

#include "h2_lvgl_display.h"
#include "h2_lvgl_platform.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

namespace h2::desktop {

struct StatusSurface {
  OwnedDisplay *provider = nullptr;
  h2_lvgl_display_t *display = nullptr;
  lv_obj_t *app_label = nullptr;
  lv_obj_t *state_label = nullptr;
  bool platform_initialized = false;
  bool lvgl_initialized = false;
  bool tick_started = false;
  std::uint64_t last_tick_ms = 0u;
  std::string app_name;
};

namespace {

void refresh(StatusSurface *surface) {
  std::uint64_t now_ms = 0u;
  if (surface == nullptr || surface->display == nullptr ||
      h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &now_ms) !=
          H2_PAL_OK) {
    return;
  }
  std::uint32_t elapsed = 0u;
  if (surface->tick_started && now_ms >= surface->last_tick_ms) {
    elapsed = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        now_ms - surface->last_tick_ms,
        std::numeric_limits<std::uint32_t>::max()));
  }
  surface->tick_started = true;
  surface->last_tick_ms = now_ms;
  lv_tick_inc(elapsed);
  (void)lv_timer_handler();
  lv_refr_now(h2_lvgl_display_lvgl(surface->display));
}

void destroy(StatusSurface *surface) {
  if (surface == nullptr) {
    return;
  }
  h2_lvgl_display_destroy(surface->display);
  if (surface->lvgl_initialized && lv_is_initialized()) {
    lv_deinit();
  }
  if (surface->platform_initialized) {
    h2_lvgl_platform_deinit();
  }
  delete surface;
}

} // namespace

OwnedStatusSurface::~OwnedStatusSurface() { destroy(handle); }

int open_status_surface(OwnedDisplay *display, const char *app_name,
                        OwnedStatusSurface *surface) {
  if (display == nullptr || display->handle == nullptr || app_name == nullptr ||
      app_name[0] == '\0' || surface == nullptr || surface->handle != nullptr) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  if (lv_is_initialized()) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  auto *state = new (std::nothrow) StatusSurface();
  if (state == nullptr) {
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  state->provider = display;
  try {
    state->app_name = app_name;
  } catch (...) {
    destroy(state);
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  const h2_lvgl_platform_config_t platform = {
      h2_desktop_platform_default_allocator(), h2_desktop_platform_task_api(),
      h2_desktop_platform_sync_api(),          h2_desktop_platform_queue_api(),
      h2_desktop_platform_time_api(),
  };
  if (h2_lvgl_platform_init(&platform) != 0) {
    destroy(state);
    return H2_DISPLAY_ERR_UNAVAILABLE;
  }
  state->platform_initialized = true;
  lv_init();
  state->lvgl_initialized = true;
  const h2_lvgl_display_config_t display_config = {
      display->display(), h2_desktop_platform_default_allocator(), 32u};
  if (h2_lvgl_display_create(&display_config, &state->display) != H2_PAL_OK) {
    destroy(state);
    return H2_DISPLAY_ERR_UNAVAILABLE;
  }
  lv_obj_t *screen = lv_obj_create(nullptr);
  state->app_label = screen == nullptr ? nullptr : lv_label_create(screen);
  state->state_label = screen == nullptr ? nullptr : lv_label_create(screen);
  if (screen == nullptr || state->app_label == nullptr ||
      state->state_label == nullptr) {
    destroy(state);
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), LV_PART_MAIN);
  lv_obj_set_style_text_color(screen, lv_color_hex(0xf3f5f7), LV_PART_MAIN);
  lv_obj_set_style_text_font(state->app_label, &lv_font_montserrat_42,
                             LV_PART_MAIN);
  lv_obj_set_style_text_font(state->state_label, &lv_font_montserrat_42,
                             LV_PART_MAIN);
  const int32_t label_width = std::max<int32_t>(
      1, lv_display_get_horizontal_resolution(
             h2_lvgl_display_lvgl(state->display)) -
             32);
  lv_obj_set_width(state->app_label, label_width);
  lv_obj_set_width(state->state_label, label_width);
  lv_obj_set_style_text_align(state->app_label, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_set_style_text_align(state->state_label, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_label_set_long_mode(state->app_label, LV_LABEL_LONG_MODE_WRAP);
  lv_label_set_long_mode(state->state_label, LV_LABEL_LONG_MODE_WRAP);
  lv_label_set_text(state->app_label, app_name);
  lv_label_set_text(state->state_label, "No Display - Running");
  lv_obj_align(state->app_label, LV_ALIGN_CENTER, 0, -28);
  lv_obj_align(state->state_label, LV_ALIGN_CENTER, 0, 28);
  lv_screen_load(screen);
  try {
    const std::string title = state->app_name + " - No Display - Running";
    h2_sdl3_set_window_title(display->handle, title.c_str());
  } catch (...) {
    destroy(state);
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  surface->handle = state;
  refresh(state);
  return H2_DISPLAY_OK;
}

int set_process_state(OwnedStatusSurface *surface, ProcessState state) {
  if (surface == nullptr || surface->handle == nullptr ||
      surface->handle->state_label == nullptr) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  const char *text = nullptr;
  switch (state) {
  case ProcessState::kRunning:
    text = "No Display - Running";
    break;
  case ProcessState::kStopping:
    text = "No Display - Stopping";
    break;
  case ProcessState::kFailed:
    text = "No Display - Failed";
    break;
  default:
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  std::string title;
  try {
    title = surface->handle->app_name + " - " + text;
  } catch (...) {
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  lv_label_set_text(surface->handle->state_label, text);
  h2_sdl3_set_window_title(surface->handle->provider->handle, title.c_str());
  refresh(surface->handle);
  return H2_DISPLAY_OK;
}

} // namespace h2::desktop
