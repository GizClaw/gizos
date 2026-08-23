#include "h2_desktop_app_support.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_smoke_display.h"
#include "layout_config.h"

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

} // namespace

int main() {
  h2::desktop::OwnedDisplay display_provider;
  h2::desktop::OwnedStatusSurface status;
  if (h2::desktop::configure_layout(kLayout) != H2_PAL_OK ||
      h2::desktop::open_display(kLayout, &display_provider) != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: invalid provider configuration\n",
                 kLayout.app_name);
    return 1;
  }
  h2_runtime_config_t config = h2::desktop::runtime_config(nullptr);
  config.display = display_provider.display();
  h2_runtime_t *runtime = nullptr;
  const int runtime_result = h2_runtime_init(&config, &runtime);
  if (runtime_result != H2_PAL_OK || runtime == nullptr) {
    std::fprintf(stderr, "desktop %s: runtime init failed (%d)\n",
                 kLayout.app_name, runtime_result);
    return 1;
  }
  const int app_result = h2_smoke_display_run(runtime);
  if (app_result != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: app failed (%d)\n",
                 kLayout.app_name, app_result);
    if (h2::desktop::open_status_surface(
            &display_provider, "example/display", &status) !=
        H2_DISPLAY_OK) {
      h2_runtime_deinit(runtime);
      return 1;
    }
    (void)h2::desktop::set_process_state(
        &status, h2::desktop::ProcessState::kFailed);
  }
  h2::desktop::wait_for_close(&display_provider);
  (void)h2_pal_display_close(display_provider.display());
  h2_runtime_deinit(runtime);
  return app_result == H2_DISPLAY_OK ? 0 : 1;
}
