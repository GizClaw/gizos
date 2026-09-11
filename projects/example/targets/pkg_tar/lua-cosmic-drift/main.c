#include "h2_lua_cosmic_drift.h"
#include "h2_web_app_host.h"

// Escape is the Back Button; its action cancels the Lua job and ends the App.
static const h2_web_app_host_button_t k_buttons[] = {
    {H2_LUA_COSMIC_DRIFT_COMPONENT_BACK, "Escape"}};

static h2_pal_result_t run_cosmic_drift(h2_web_app_host_t *host,
                                        h2_runtime_t *runtime, void *user) {
  (void)user;
  const h2_lua_cosmic_drift_config_t config = {
      .back_component_id = H2_LUA_COSMIC_DRIFT_COMPONENT_BACK,
      .should_stop = h2_web_app_host_should_stop,
      .should_stop_user = host,
      .on_ready = h2_web_app_host_ready,
      .on_ready_user = host,
  };
  return h2_lua_cosmic_drift_run(runtime, &config);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "lua-cosmic-drift",
      .display_width = 240,
      .display_height = 240,
      .buttons = k_buttons,
      .button_count = 1u,
      .stack_size = 262144u,
  };
  return h2_web_app_host_run(&config, run_cosmic_drift, NULL);
}
