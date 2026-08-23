#include "h2_desktop_app_support.h"
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
  h2::desktop::OwnedDisplay *display = nullptr;
  h2::starboy::MouseTouchAdapter *mouse_touch = nullptr;
  h2_sdl3_pointer_state_t pointer = {};
  bool global_mouse_checked = false;
  bool global_mouse_available = false;
};

int should_stop(void *user) {
  auto *context = static_cast<AppContext *>(user);
  if (context == nullptr || context->display == nullptr ||
      context->mouse_touch == nullptr ||
      h2::desktop::poll_events(context->display) != 0) {
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
  } else if (h2::desktop::read_pointer(
                 context->display, &context->pointer) == H2_PAL_OK) {
    // The native pressed bit is deliberately ignored: hover drives gaze.
    context->mouse_touch->update(
        true, context->pointer.x, context->pointer.y);
  } else {
    context->mouse_touch->update(false, 0, 0);
  }
  return 0;
}

}  // namespace

int main() {
  h2::desktop::OwnedDisplay display;
  h2::desktop::OwnedAudio audio;
  if (h2::desktop::configure_layout(kLayout) != H2_PAL_OK ||
      h2::desktop::open_display(kLayout, &display) != H2_DISPLAY_OK ||
      h2::desktop::open_audio(true, &audio) != H2_AUDIO_OK) {
    std::fprintf(stderr,
                 "desktop %s: SDL display or real microphone unavailable\n",
                 kLayout.app_name);
    return 1;
  }

  h2::starboy::MouseTouchAdapter mouse_touch(
      static_cast<std::uint32_t>(kLayout.width),
      static_cast<std::uint32_t>(kLayout.height));

  h2_runtime_config_t runtime_config = h2::desktop::runtime_config(nullptr);
  runtime_config.display = display.display();
  runtime_config.touch = mouse_touch.api();
  runtime_config.audio = audio.api();
  h2_runtime_t *runtime = nullptr;
  const h2_pal_result_t runtime_result =
      h2_runtime_init(&runtime_config, &runtime);
  if (runtime_result != H2_PAL_OK || runtime == nullptr) {
    std::fprintf(stderr, "desktop %s: Runtime init failed (%d)\n",
                 kLayout.app_name, runtime_result);
    return 1;
  }

  AppContext context;
  context.display = &display;
  context.mouse_touch = &mouse_touch;
  h2_starboy_config_t config = {};
  config.frame_interval_ms = 16u;
  config.random_seed = static_cast<std::uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  config.initial_pupil_style = H2_STARBOY_PUPIL_STYLE_ACORN;
  config.should_stop = should_stop;
  config.should_stop_user = &context;
  const int result = h2_starboy_run(runtime, &config);
  mouse_touch.update(false, context.pointer.x, context.pointer.y);
  // The App owns the single Display open/close pair. Pump once more on the
  // same thread so the SDL provider applies its deferred native teardown.
  (void)h2::desktop::poll_events(&display);
  h2_runtime_deinit(runtime);
  if (result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: App exited with result %d\n",
                 kLayout.app_name, result);
    return 1;
  }
  return 0;
}
