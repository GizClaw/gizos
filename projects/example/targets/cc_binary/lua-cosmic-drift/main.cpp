#include "h2_desktop_application_host.hpp"
#include "h2_lua_cosmic_drift.h"
#include "layout_config.h"
#include "pointer_touch_adapter.h"

#include <cstdio>

namespace {

constexpr h2_runtime_component_id_t kBackComponentId =
    H2_LUA_COSMIC_DRIFT_COMPONENT_BACK;

constexpr h2::desktop::Layout kLayout = {
    h2_desktop_layout::app_name,        h2_desktop_layout::title,
    h2_desktop_layout::width,           h2_desktop_layout::height,
    h2_desktop_layout::mounts,          h2_desktop_layout::mount_count,
    h2_desktop_layout::peripherals,     h2_desktop_layout::peripheral_count,
    h2_desktop_layout::normalized_json,
};

constexpr h2_runtime_component_mapping_entry_t mappings[] = {{H2_LUA_COSMIC_DRIFT_COMPONENT_BACK, 1u}};

} // namespace

static h2::cosmic_drift::PointerTouchAdapter pointer_touch(kLayout.width, kLayout.height);
static h2_application_host_t *app_host;
static int should_stop(void *user) {
  if (h2_application_host_should_stop(user)) return 1;
  h2_sdl3_pointer_state_t pointer{};
  if (h2_desktop_application_host_pointer(app_host, &pointer) == H2_PAL_OK)
    pointer_touch.update(pointer.x, pointer.y, pointer.pressed != 0);
  return 0;
}
static h2_pal_result_t configure(void *, h2_runtime_config_t *config) {
  config->touch = pointer_touch.api();
  return H2_PAL_OK;
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *) {
  app_host = host;
  int opened = h2_pal_display_open(runtime->display);
  if (opened != H2_PAL_OK) return opened;
  const h2_lua_cosmic_drift_config_t config = {
      .back_component_id = kBackComponentId,
      .hover_control = 1,
      .should_stop = should_stop,
      .should_stop_user = host,
      .on_ready = h2_application_host_ready,
      .on_ready_user = host,
  };
  return h2_lua_cosmic_drift_run(runtime, &config);
}
int main() {
  auto platform = h2::desktop::application_host_config(kLayout, 0u);
  platform.input = 1;
  platform.entry_on_main = 0;
  platform.mappings = mappings;
  platform.mapping_count = 1u;
  h2_application_host_config_t app{};
  app.entry = run;
  app.configure = configure;
  h2_application_host_result_t result{};
  return h2_desktop_application_host_run(&platform, &app, &result) == H2_PAL_OK ? 0 : 1;
}
