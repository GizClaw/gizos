#include "layout_config.h"
#include "player.h"

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
  return h2_desktop_mp4_player_main(
      kLayout,
      "projects/example/apps/mp4-player/data/media/"
      "test_1024x600_h264_aac.mp4");
}
