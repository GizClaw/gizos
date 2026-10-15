#include "h2_web_fs.h"
#include "h2_web_gizclaw_apps.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <assert.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

static const h2_pal_task_api_t *base_task;
static int inject_join_failure, close_fixture;
static int start_task(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *context,
                      h2_pal_task_t **out) {
  (void)user;
  return h2_pal_task_start(base_task, options, entry, context, out);
}
static int join_task(void *user, h2_pal_task_t *task) {
  (void)user;
  if (inject_join_failure) {
    inject_join_failure = 0;
    return H2_PAL_ERR_TIMEOUT;
  }
  return h2_pal_task_join(base_task, task);
}
static const h2_pal_task_vtable_t task_vtable = {start_task, join_task};
static const h2_pal_task_api_t tasks = {.vtable = &task_vtable};
/* clang-format off */
EM_JS(void, fixture_mode, (void *c, h2_web_main_result_t *r, h2_web_main_completion_t *done), {
  h2WebMain(c, r, done, [], "i32", () => Number(new URLSearchParams(location.search).get('close') || 0));
});
EM_JS(void, closed_sdk, (void *c, h2_web_main_result_t *r, h2_web_main_completion_t *done), {
  h2WebMain(c, r, done, [], "i32", async () => {
    try { await globalThis.h2WebLuaApps.list(); return 1; } catch (error) { return error.code; }
  });
});
EM_JS(void, kv_seeded, (void *c, h2_web_main_result_t *r, h2_web_main_completion_t *done), {
  h2WebMain(c, r, done, [], "i32", () => Number(localStorage.getItem('h2-catalog-test-user-kv') || 0));
});
EM_JS(void, kv_mark_seeded, (void *c, h2_web_main_result_t *r, h2_web_main_completion_t *done), {
  h2WebMain(c, r, done, [], null, () => localStorage.setItem('h2-catalog-test-user-kv', '1'));
});
EM_JS(void, page_stop, (void *c, h2_web_main_result_t *r, h2_web_main_completion_t *done), {
  h2WebMain(c, r, done, [], "i32", () => Module.h2WebStopRequested ? 1 : 0);
});
/* clang-format on */
static void check_user_kv(const h2_pal_fs_api_t *fs) {
  const char *path = "/data/gizclaw/user/demo/.kv";
  h2_pal_fs_file_t *file = NULL;
  size_t count = 0;
  if (!h2_web_main_call(kv_seeded, NULL).i32) {
    assert(h2_pal_fs_mkdir(fs, "/data/gizclaw/user") == H2_PAL_OK);
    assert(h2_pal_fs_mkdir(fs, "/data/gizclaw/user/demo") == H2_PAL_OK);
    assert(h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) ==
           H2_PAL_OK);
    assert(h2_pal_fs_write(fs, file, "save", 4u, &count) == H2_PAL_OK &&
           count == 4u);
    assert(h2_pal_fs_sync(fs, file) == H2_PAL_OK);
    assert(h2_pal_fs_close(fs, file) == H2_PAL_OK);
    (void)h2_web_main_call(kv_mark_seeded, NULL);
  }
  char value[4];
  assert(h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_READ, &file) == H2_PAL_OK);
  assert(h2_pal_fs_read(fs, file, value, sizeof(value), &count) == H2_PAL_OK &&
         count == 4u);
  assert(memcmp(value, "save", 4u) == 0);
  assert(h2_pal_fs_close(fs, file) == H2_PAL_OK);
}

int main(void) {
  close_fixture = h2_web_main_call(fixture_mode, NULL).i32;
  inject_join_failure = close_fixture;
  const h2_web_platform_config_t platform_config = {240u, 240u};
  h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
  if (platform == NULL)
    return 1;
  const char *const readonly[] = {"/data/lua"};
  const h2_web_fs_config_t fs_config = {.persistent_root = "/data/gizclaw",
                                        .readonly_roots = readonly,
                                        .readonly_root_count = 1u};
  h2_web_fs_t *fs = NULL;
  h2_gizclaw_lua_app_catalog_t *apps = NULL;
  h2_web_gizclaw_apps_t *bridge = NULL;
  int rc = h2_web_fs_open(platform, &fs_config, &fs);
  const h2_pal_fs_api_t *files = h2_web_fs_api(fs);
  if (rc == H2_PAL_OK) {
    check_user_kv(files);
    const h2_gizclaw_lua_app_catalog_config_t config = {
        .allocator = h2_web_platform_mem_api(),
        .fs = files,
        .sync = h2_web_platform_sync_api(platform),
        .preinstalled_index = "/data/lua/preinstalled.json",
        .install_root = "/data/gizclaw/lua_apps"};
    rc = h2_gizclaw_lua_app_catalog_create(&config, &apps);
  }
  if (rc == H2_PAL_OK) {
    base_task = h2_web_platform_task_api(platform);
    const h2_web_gizclaw_apps_config_t config = {
        .apps = apps,
        .allocator = h2_web_platform_mem_api(),
        .queue = h2_web_platform_queue_api(platform),
        .task = close_fixture ? &tasks : base_task};
    rc = h2_web_gizclaw_apps_open(&config, &bridge);
  }
  if (rc == H2_PAL_OK) {
    puts("H2_WEB_LUA_APPS ready");
    while (!h2_web_main_call(page_stop, NULL).i32) {
      rc = h2_pal_time_sleep_ms(h2_web_platform_time_api(platform), 10u);
      if (rc != H2_PAL_OK)
        break;
    }
  }
  int cleanup = h2_web_gizclaw_apps_close(&bridge);
  if (close_fixture) {
    assert(cleanup == H2_PAL_ERR_TIMEOUT && bridge != NULL);
    assert(h2_gizclaw_lua_app_catalog_destroy(apps) == H2_PAL_ERR_BUSY);
    assert(h2_web_main_call(closed_sdk, NULL).i32 == H2_PAL_ERR_CLOSED);
    cleanup = h2_web_gizclaw_apps_close(&bridge);
    assert(cleanup == H2_PAL_OK && bridge == NULL);
    puts("WEB_LUA_APPS close_retry=PASS");
  }
  if (cleanup == H2_PAL_OK) {
    cleanup = h2_gizclaw_lua_app_catalog_destroy(apps);
    if (cleanup == H2_PAL_OK)
      apps = NULL;
  }
  if (cleanup != H2_PAL_OK) {
    printf("H2_WEB_APP name=web-lua-apps result=FAIL rc=%d retained=1\n",
           cleanup);
    return 1;
  }
  const int closed = h2_web_fs_close(fs);
  if (closed != H2_PAL_OK) {
    printf("H2_WEB_APP name=web-lua-apps result=FAIL rc=%d retained=1\n",
           closed);
    return 1;
  }
  const int destroyed = h2_web_platform_destroy(platform);
  if (rc == H2_PAL_OK)
    rc = destroyed;
  printf("H2_WEB_APP name=web-lua-apps result=%s rc=%d fs=%d destroy=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, closed, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;
}
