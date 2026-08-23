#include "h2_desktop_app_support.h"
#include "h2_libco_smoke.h"
#include "layout_config.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr h2::desktop::Layout kLayout = {
    h2_desktop_layout::app_name,        h2_desktop_layout::title,
    h2_desktop_layout::width,           h2_desktop_layout::height,
    h2_desktop_layout::mounts,          h2_desktop_layout::mount_count,
    h2_desktop_layout::peripherals,     h2_desktop_layout::peripheral_count,
    h2_desktop_layout::normalized_json,
};

} // namespace

int main() {
  h2::desktop::OwnedDisplay display;
  h2::desktop::OwnedStatusSurface status;
  if (h2::desktop::configure_layout(kLayout) != H2_PAL_OK ||
      h2::desktop::open_display(kLayout, &display) != H2_DISPLAY_OK ||
      h2::desktop::open_status_surface(&display, "e2e/libco", &status) !=
          H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: provider initialization failed\n",
                 kLayout.app_name);
    return 1;
  }

  h2_runtime_config_t config = h2::desktop::runtime_config(nullptr);
  h2_runtime_t *runtime = nullptr;
  const int runtime_result = h2_runtime_init(&config, &runtime);
  int app_result = runtime_result;
  if (runtime_result == H2_PAL_OK && runtime != nullptr) {
    const h2_libco_smoke_config_t failure_config = {
        H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE + 1u,
        H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS,
    };
    const h2_libco_smoke_config_t *smoke_config =
        std::getenv("H2_LIBCO_SMOKE_INJECT_FAILURE") == nullptr
            ? nullptr
            : &failure_config;
    app_result = h2_libco_smoke_run(runtime, smoke_config);
  }
  if (app_result == H2_PAL_OK) {
    std::printf("H2_DESKTOP_LIBCO_SMOKE_READY rc=0\n");
    std::fflush(stdout);
    (void)h2::desktop::set_process_state(
        &status, h2::desktop::ProcessState::kRunning);
  } else {
    std::fprintf(stderr, "H2_DESKTOP_LIBCO_SMOKE_FAIL rc=%d\n", app_result);
    (void)h2::desktop::set_process_state(
        &status, h2::desktop::ProcessState::kFailed);
  }

  if (std::getenv("H2_LIBCO_SMOKE_AUTO_CLOSE") == nullptr) {
    h2::desktop::wait_for_close(&display);
  }
  if (runtime != nullptr) {
    h2_runtime_deinit(runtime);
  }
  return app_result == H2_PAL_OK ? 0 : 1;
}
