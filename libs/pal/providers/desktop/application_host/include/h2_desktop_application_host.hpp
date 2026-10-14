#ifndef H2_DESKTOP_APPLICATION_HOST_HPP
#define H2_DESKTOP_APPLICATION_HOST_HPP

#include "h2_desktop_app_support.h"
#include "h2_desktop_application_host.h"

namespace h2::desktop {
/** Translate generated Desktop layout data into the shared owner config. */
inline h2_desktop_application_host_config_t
application_host_config(const Layout &layout, unsigned services = 0u) {
  h2_desktop_application_host_config_t config{};
  config.window = {layout.title, layout.width, layout.height};
  config.mounts = layout.mounts;
  config.mount_count = layout.mount_count;
  config.peripherals = layout.peripherals;
  config.peripheral_count = layout.peripheral_count;
  config.services = services;
  config.entry_on_main = 1;
  return config;
}
} // namespace h2::desktop
#endif
