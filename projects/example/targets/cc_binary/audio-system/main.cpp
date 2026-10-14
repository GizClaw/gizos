#include "h2_desktop_application_host.hpp"
#include "h2_smoke_audio_system.h"
#include "layout_config.h"

#include <cstdio>

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
  const h2_smoke_audio_system_config_t config = {"/data/audio/music_loop.ogg", 0u};
  int rc = h2_smoke_audio_system_run(runtime, &config);
  if (rc == H2_PAL_OK) {
    h2_application_host_ready(host);
    rc = h2_desktop_application_host_wait(host);
  }
  int stopped = h2_smoke_audio_system_stop();
  if (stopped != H2_PAL_OK) {
    h2_application_host_retain(host, nullptr, stopped);
    return stopped;
  }
  return rc;
}
int main() {
  auto platform = h2::desktop::application_host_config(kLayout, H2_DESKTOP_HOST_AUDIO);
  platform.require_real_audio = 1;
  platform.status_name = "example/audio-system";
  h2_application_host_config_t app{};
  app.entry = run;
  h2_application_host_result_t result{};
  return h2_desktop_application_host_run(&platform, &app, &result) == H2_PAL_OK ? 0 : 1;
}
