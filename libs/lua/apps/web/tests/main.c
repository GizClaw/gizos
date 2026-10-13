#include "h2_web_app_host.h"
#include "h2_web_lua_apps.h"
#include <stdio.h>

static struct {
  h2_lua_apps_t *apps;
  h2_web_lua_apps_t *bridge;
} owner;

static h2_pal_result_t run(h2_web_app_host_t *host, h2_runtime_t *runtime,
                           void *user) {
  (void)user;
  const h2_lua_apps_config_t config = {
      .allocator = runtime->mem,
      .fs = runtime->fs,
      .sync = runtime->sync,
      .preinstalled_index = "/data/lua/preinstalled.json",
      .install_root = "/data/gizclaw/lua_apps"};
  int rc = h2_lua_apps_create(&config, &owner.apps);
  if (rc == H2_PAL_OK) {
    const h2_web_lua_apps_config_t bridge = {.apps = owner.apps,
                                             .allocator = runtime->mem,
                                             .queue = runtime->queue,
                                             .task = runtime->task};
    rc = h2_web_lua_apps_open(&bridge, &owner.bridge);
  }
  if (rc == H2_PAL_OK) {
    puts("H2_WEB_LUA_APPS ready");
    while (!h2_web_app_host_should_stop(host))
      h2_pal_time_sleep_ms(runtime->time, 10u);
  }
  int cleanup = h2_web_lua_apps_close(&owner.bridge);
  if (cleanup == H2_PAL_OK)
    cleanup = h2_lua_apps_destroy(owner.apps);
  if (cleanup != H2_PAL_OK) {
    (void)h2_web_app_host_quarantine(host, &owner, cleanup);
    return cleanup;
  }
  owner.apps = NULL;
  return rc;
}

int main(void) {
  static const char *const readonly[] = {"/data/lua"};
  const h2_web_app_host_config_t config = {.name = "web-lua-apps",
                                           .persistent_root = "/data/gizclaw",
                                           .readonly_roots = readonly,
                                           .readonly_root_count = 1u,
                                           .managed_shutdown = 1};
  return h2_web_app_host_run(&config, run, NULL);
}
