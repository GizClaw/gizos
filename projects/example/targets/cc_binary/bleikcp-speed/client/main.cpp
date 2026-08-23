#include "bleikcp-speed-native.h"
#include "layout_config.h"

int main() {
  constexpr h2::desktop::Layout layout = {
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
  return h2::desktop::run_bleikcp_speed(layout, true);
}
