#include "h2_desktop_application_host.hpp"
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

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *) {
    const h2_libco_smoke_config_t failure_config = {
        H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE + 1u,
        H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS,
    };
    const h2_libco_smoke_config_t *smoke_config =
        std::getenv("H2_LIBCO_SMOKE_INJECT_FAILURE") == nullptr
            ? nullptr
            : &failure_config;
  int rc = h2_libco_smoke_run(runtime, smoke_config);
  if (rc == H2_PAL_OK) {
    std::printf("H2_DESKTOP_LIBCO_SMOKE_READY rc=0\n");
    h2_application_host_ready(host);
    if (std::getenv("H2_LIBCO_SMOKE_AUTO_CLOSE") == nullptr)
      return h2_desktop_application_host_wait(host);
  } else std::fprintf(stderr, "H2_DESKTOP_LIBCO_SMOKE_FAIL rc=%d\n", rc);
  return rc;
}
int main() {
  auto platform = h2::desktop::application_host_config(kLayout);
  if (std::getenv("H2_LIBCO_SMOKE_AUTO_CLOSE") == nullptr) platform.status_name = "e2e/libco";
  h2_application_host_config_t app{};
  app.entry = run;
  h2_application_host_result_t result{};
  return h2_desktop_application_host_run(&platform, &app, &result) == H2_PAL_OK ? 0 : 1;
}
