#include "h2_desktop_app_support.h"
#include "h2_qrcode_example.h"
#include "layout_config.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr const char *kDefaultText = "https://github.com/GizClaw/gizos";

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

int main(int argc, char **argv) {
  // The first argument selects the encoded payload so the window can show an
  // arbitrary string without rebuilding.
  const char *text = argc > 1 ? argv[1] : kDefaultText;
  h2::desktop::OwnedDisplay display_provider;
  if (h2::desktop::configure_layout(kLayout) != H2_PAL_OK ||
      h2::desktop::open_display(kLayout, &display_provider) != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: invalid provider configuration\n",
                 kLayout.app_name);
    return 1;
  }
  h2_runtime_config_t config = h2::desktop::runtime_config(nullptr);
  config.display = display_provider.display();
  h2_runtime_t *runtime = nullptr;
  const h2_pal_result_t runtime_result = h2_runtime_init(&config, &runtime);
  if (runtime_result != H2_PAL_OK || runtime == nullptr) {
    std::fprintf(stderr, "desktop %s: runtime init failed (%d)\n",
                 kLayout.app_name, runtime_result);
    return 1;
  }
  const h2_qrcode_example_config_t app_config = {
      text, H2_QRCODE_ECC_MEDIUM, 0, 0, 0u, nullptr, nullptr,
  };
  const h2_pal_result_t app_result =
      h2_qrcode_example_run(runtime, &app_config);
  if (app_result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: app failed (%d)\n", kLayout.app_name,
                 app_result);
  } else {
    std::printf("H2_QRCODE_EXAMPLE_READY text=%s\n", text);
  }
  h2::desktop::wait_for_close(&display_provider);
  (void)h2_pal_display_close(display_provider.display());
  h2_runtime_deinit(runtime);
  return app_result == H2_PAL_OK ? 0 : 1;
}
