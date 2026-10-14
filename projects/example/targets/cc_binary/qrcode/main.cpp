#include "h2_desktop_application_host.hpp"
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

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  const char *text = static_cast<const char *>(user);
  const h2_qrcode_example_config_t config = {text, H2_QRCODE_ECC_MEDIUM, 0, 0, 0u, nullptr, nullptr};
  int rc = h2_qrcode_example_run(runtime, &config);
  if (rc != H2_PAL_OK) return rc;
  std::printf("H2_QRCODE_EXAMPLE_READY text=%s\n", text);
  h2_application_host_ready(host);
  return h2_desktop_application_host_wait(host);
}
int main(int argc, char **argv) {
  const auto platform = h2::desktop::application_host_config(kLayout);
  h2_application_host_config_t app{};
  app.entry = run;
  app.user = const_cast<char *>(argc > 1 ? argv[1] : kDefaultText);
  h2_application_host_result_t result{};
  return h2_desktop_application_host_run(&platform, &app, &result) == H2_PAL_OK ? 0 : 1;
}
