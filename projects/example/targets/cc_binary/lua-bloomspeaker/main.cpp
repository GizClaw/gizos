#include "h2_desktop_application_host.hpp"
#include "h2_bloomspeaker.h"
#include "layout_config.h"

#include <cstdio>

namespace {

constexpr h2_runtime_component_id_t kPowerComponentId =
    H2_BLOOMSPEAKER_COMPONENT_POWER;

constexpr h2::desktop::Layout kLayout = {
    h2_desktop_layout::app_name,        h2_desktop_layout::title,
    h2_desktop_layout::width,           h2_desktop_layout::height,
    h2_desktop_layout::mounts,          h2_desktop_layout::mount_count,
    h2_desktop_layout::peripherals,     h2_desktop_layout::peripheral_count,
    h2_desktop_layout::normalized_json,
};

constexpr h2_runtime_component_mapping_entry_t mappings[] = {{H2_BLOOMSPEAKER_COMPONENT_POWER, 2u}};

} // namespace

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *) {
  const h2_bloomspeaker_config_t config = {
      .back_component_id = kPowerComponentId,
      .power_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
      .pairing_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
      .should_stop = h2_application_host_should_stop,
      .should_stop_user = host,
      .on_ready = h2_application_host_ready,
      .on_ready_user = host,
      .pause_management_advertising = nullptr,
      .resume_management_advertising = nullptr,
      .management_advertising_user = nullptr,
  };
  return h2_bloomspeaker_run(runtime, &config);
}
int main() {
  auto platform = h2::desktop::application_host_config(kLayout,
      H2_DESKTOP_HOST_AUDIO | H2_DESKTOP_HOST_CRYPTO | H2_DESKTOP_HOST_BLEIKCP);
  platform.input = 1;
  platform.entry_on_main = 0;
  platform.mappings = mappings;
  platform.mapping_count = 1u;
  h2_application_host_config_t app{};
  app.entry = run;
  h2_application_host_result_t result{};
  return h2_desktop_application_host_run(&platform, &app, &result) == H2_PAL_OK ? 0 : 1;
}
