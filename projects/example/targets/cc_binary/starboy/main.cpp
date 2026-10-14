#include "h2_desktop_application_host.hpp"
#include "h2_starboy.h"
#include "layout_config.h"
#include "mouse_touch_adapter.h"

#include <chrono>
#include <cstdio>
#include <cstdint>

namespace {

constexpr h2::desktop::Layout kLayout = {
    h2_desktop_layout::app_name,
    h2_desktop_layout::title,
    h2_desktop_layout::width,
    h2_desktop_layout::height,
    h2_desktop_layout::mounts,
    h2_desktop_layout::mount_count,
    h2_desktop_layout::peripherals,
    h2_desktop_layout::peripheral_count,
    h2_desktop_layout::normalized_json,
};

struct AppContext {
  h2_application_host_t *host = nullptr;
  h2::starboy::MouseTouchAdapter *mouse_touch = nullptr;
  h2_sdl3_pointer_state_t pointer = {};
  bool global_mouse_checked = false;
  bool global_mouse_available = false;
};

int should_stop(void *user) {
  auto *context = static_cast<AppContext *>(user);
  if (context == nullptr || context->host == nullptr ||
      context->mouse_touch == nullptr ||
      h2_desktop_application_host_should_stop(context->host) != 0) {
    return 1;
  }
  if (!context->global_mouse_checked) {
    context->global_mouse_available =
        context->mouse_touch->bind_global_window(kLayout.title);
    context->global_mouse_checked = true;
  }
  if (context->global_mouse_available &&
      context->mouse_touch->sample_global_mouse()) {
    // Global desktop coordinates keep gaze active outside the app window.
  } else if (h2_desktop_application_host_pointer(
                 context->host, &context->pointer) == H2_PAL_OK) {
    // The native pressed bit is deliberately ignored: hover drives gaze.
    context->mouse_touch->update(
        true, context->pointer.x, context->pointer.y);
  } else {
    context->mouse_touch->update(false, 0, 0);
  }
  return 0;
}

}  // namespace

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  config->touch = static_cast<AppContext *>(user)->mouse_touch->api();
  return H2_PAL_OK;
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  auto *context = static_cast<AppContext *>(user);
  context->host = host;
  h2_starboy_config_t config = {};
  config.frame_interval_ms = 16u;
  config.random_seed = static_cast<std::uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  config.initial_pupil_style = H2_STARBOY_PUPIL_STYLE_ACORN;
  config.should_stop = should_stop;
  config.should_stop_user = context;
  const int result = h2_starboy_run(runtime, &config);
  context->mouse_touch->update(false, context->pointer.x, context->pointer.y);
  return result;
}
int main() {
  static h2::starboy::MouseTouchAdapter mouse_touch(kLayout.width, kLayout.height);
  static AppContext context;
  context.mouse_touch = &mouse_touch;
  auto platform = h2::desktop::application_host_config(kLayout, H2_DESKTOP_HOST_AUDIO);
  platform.require_real_audio = 1;
  h2_application_host_config_t app{};
  app.entry = run;
  app.user = &context;
  app.configure = configure;
  h2_application_host_result_t result{};
  return h2_desktop_application_host_run(&platform, &app, &result) == H2_PAL_OK ? 0 : 1;
}
