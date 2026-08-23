#include "h2_desktop_tap_reset_app.h"

#include "h2_desktop_app_support.h"
#include "h2_lvgl_platform.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_tap_reset.h"
#include "layout_config.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

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
  h2_runtime_t *runtime;
  h2::desktop::OwnedDisplay *display;
  std::atomic<bool> stop{false};
  std::atomic<bool> finished{false};
  h2_pal_result_t result = H2_PAL_OK;
};

h2_pal_result_t read_pointer(void *user,
                            h2_tap_reset_pointer_state_t *out_state) {
  if (user == nullptr || out_state == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_sdl3_pointer_state_t pointer = {};
  const int result = h2::desktop::read_pointer(
      static_cast<h2::desktop::OwnedDisplay *>(user), &pointer);
  if (result != H2_PAL_OK) {
    return static_cast<h2_pal_result_t>(result);
  }
  *out_state = {pointer.x, pointer.y, pointer.pressed};
  return H2_PAL_OK;
}

int should_stop(void *user) {
  const auto *context = static_cast<AppContext *>(user);
  return context == nullptr || context->stop.load(std::memory_order_acquire);
}

void app_main(AppContext *context) {
  const h2_lvgl_platform_config_t platform = {
      context->runtime->mem,
      context->runtime->task,
      context->runtime->sync,
      context->runtime->queue,
      context->runtime->time,
  };
  if (h2_lvgl_platform_init(&platform) != 0) {
    context->result = H2_PAL_ERR_UNAVAILABLE;
    context->finished.store(true, std::memory_order_release);
    return;
  }
  const h2_tap_reset_config_t config = {
      "FIRMWARES / MACOS HOST",
      "LVGL UI\nRuntime + PAL boundary\nSDL display and pointer",
      "Click from macOS",
      read_pointer,
      context->display,
      should_stop,
      context,
  };
  context->result = h2_tap_reset_run(context->runtime, &config);
  h2_lvgl_platform_deinit();
  context->finished.store(true, std::memory_order_release);
}

} // namespace

int h2_desktop_tap_reset_app_run(void) {
  h2::desktop::OwnedDisplay display_provider;
  if (h2::desktop::configure_layout(kLayout) != H2_PAL_OK ||
      h2::desktop::open_display(kLayout, &display_provider) != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop tap-reset: platform configuration failed\n");
    return 1;
  }

  h2_runtime_config_t config = h2::desktop::runtime_config(nullptr);
  config.display = display_provider.display();
  config.system_event = h2_pal_unsupported_system_event_api();
  h2_runtime_t *runtime = nullptr;
  const h2_pal_result_t runtime_result = h2_runtime_init(&config, &runtime);
  if (runtime_result != H2_PAL_OK || runtime == nullptr) {
    std::fprintf(stderr, "desktop tap-reset: runtime init failed (%d)\n",
                 runtime_result);
    return 1;
  }

  h2_pal_display_t *display = display_provider.display();
  if (h2_pal_display_open(display) != H2_DISPLAY_OK) {
    h2_runtime_deinit(runtime);
    std::fprintf(stderr, "desktop tap-reset: display init failed\n");
    return 1;
  }

  AppContext context = {};
  context.runtime = runtime;
  context.display = &display_provider;
  std::thread worker;
  try {
    worker = std::thread(app_main, &context);
  } catch (...) {
    (void)h2_pal_display_close(display);
    (void)h2::desktop::poll_events(&display_provider);
    h2_runtime_deinit(runtime);
    std::fprintf(stderr, "desktop tap-reset: App thread creation failed\n");
    return 1;
  }

  while (!context.finished.load(std::memory_order_acquire) &&
         h2::desktop::poll_events(&display_provider) == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  context.stop.store(true, std::memory_order_release);
  worker.join();
  (void)h2_pal_display_close(display);
  (void)h2::desktop::poll_events(&display_provider);

  if (context.result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop tap-reset: App exited with result %d\n",
                 context.result);
    h2_runtime_deinit(runtime);
    return 1;
  }
  h2_runtime_deinit(runtime);
  return 0;
}
