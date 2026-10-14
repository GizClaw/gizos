#include "h2_desktop_application_host.hpp"
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

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *) {
  int rc = h2_smoke_display_run(runtime);
  if (rc != H2_PAL_OK) return rc;
  h2_application_host_ready(host);
  return h2_desktop_application_host_wait(host);
}
int main() {
  auto platform = h2::desktop::application_host_config(kLayout);
  platform.status_name = "example/display";
  platform.status_on_error = 1;
  h2_application_host_config_t app{};
  app.entry = run;
  h2_application_host_result_t result{};
  return h2_desktop_application_host_run(&platform, &app, &result) == H2_PAL_OK ? 0 : 1;
}
