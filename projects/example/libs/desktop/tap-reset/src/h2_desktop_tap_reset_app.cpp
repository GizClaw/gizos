#include "h2_desktop_tap_reset_app.h"

#include "h2_desktop_app_support.h"
#include "h2_lvgl_platform.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_tap_reset.h"
#include "layout_config.h"

#include "h2_atomic.h"
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
  h2_atomic_bool_t stop{};
  h2_atomic_bool_t finished{};
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
  return context == nullptr || h2_atomic_bool_load(&context->stop, H2_ATOMIC_ACQUIRE);
}

void app_main(AppContext *context) {
  const h2_lvgl_platform_config_t platform = {
      context->runtime->mem,
      context->runtime->task,
      context->runtime->sync,
      context->runtime->queue,
      context->runtime->time,
      0u, 0u,
  };
  if (h2_lvgl_platform_init(&platform) != 0) {
    context->result = H2_PAL_ERR_UNAVAILABLE;
    h2_atomic_bool_store(&context->finished, true, H2_ATOMIC_RELEASE);
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
  h2_atomic_bool_store(&context->finished, true, H2_ATOMIC_RELEASE);
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
  if (h2_atomic_bool_init(&context.stop, false) != H2_ATOMIC_OK ||
      h2_atomic_bool_init(&context.finished, false) != H2_ATOMIC_OK) {
    h2_atomic_bool_destroy(&context.stop);
    h2_atomic_bool_destroy(&context.finished);
    (void)h2_pal_display_close(display);
    h2_runtime_deinit(runtime);
    return 1;
  }
  auto destroy_context = [&context]() {
    h2_atomic_bool_destroy(&context.stop);
    h2_atomic_bool_destroy(&context.finished);
  };
  std::thread worker;
  try {
    worker = std::thread(app_main, &context);
  } catch (...) {
    (void)h2_pal_display_close(display);
    (void)h2::desktop::poll_events(&display_provider);
    h2_runtime_deinit(runtime);
    destroy_context();
    std::fprintf(stderr, "desktop tap-reset: App thread creation failed\n");
    return 1;
  }

  while (!h2_atomic_bool_load(&context.finished, H2_ATOMIC_ACQUIRE) &&
         h2::desktop::poll_events(&display_provider) == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  h2_atomic_bool_store(&context.stop, true, H2_ATOMIC_RELEASE);
  worker.join();
  (void)h2_pal_display_close(display);
  (void)h2::desktop::poll_events(&display_provider);

  if (context.result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop tap-reset: App exited with result %d\n",
                 context.result);
    h2_runtime_deinit(runtime);
    destroy_context();
    return 1;
  }
  h2_runtime_deinit(runtime);
  destroy_context();
  return 0;
}
