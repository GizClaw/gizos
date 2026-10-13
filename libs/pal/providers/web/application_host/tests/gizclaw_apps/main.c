#include "h2_web_application_host.h"
#include "h2_web_gizclaw_apps.h"
#include "h2_web_main_thread.h"
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
/* clang-format on */
static void check_user_kv(h2_runtime_t *runtime) {
    const char *path = "/data/gizclaw/user/demo/.kv";
    h2_pal_fs_file_t *file = NULL;
    size_t count = 0;
    if (!h2_web_main_call(kv_seeded, NULL).i32) {
        assert(h2_pal_fs_mkdir(runtime->fs, "/data/gizclaw/user") == H2_PAL_OK);
        assert(h2_pal_fs_mkdir(runtime->fs, "/data/gizclaw/user/demo") ==
               H2_PAL_OK);
        assert(h2_pal_fs_open(runtime->fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                              &file) == H2_PAL_OK);
        assert(h2_pal_fs_write(runtime->fs, file, "save", 4u, &count) ==
                   H2_PAL_OK &&
               count == 4u);
        assert(h2_pal_fs_sync(runtime->fs, file) == H2_PAL_OK);
        assert(h2_pal_fs_close(runtime->fs, file) == H2_PAL_OK);
        (void)h2_web_main_call(kv_mark_seeded, NULL);
    }
    char value[4];
    assert(h2_pal_fs_open(runtime->fs, path, H2_PAL_FS_OPEN_READ, &file) ==
           H2_PAL_OK);
    assert(h2_pal_fs_read(runtime->fs, file, value, sizeof(value), &count) ==
               H2_PAL_OK &&
           count == 4u);
    assert(memcmp(value, "save", 4u) == 0);
    assert(h2_pal_fs_close(runtime->fs, file) == H2_PAL_OK);
}

static struct {
    h2_gizclaw_lua_app_catalog_t *apps;
    h2_web_gizclaw_apps_t *bridge;
} owner;

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime,
                           void *user) {
    (void)user;
    check_user_kv(runtime);
    const h2_gizclaw_lua_app_catalog_config_t config = {
        .allocator = runtime->mem,
        .fs = runtime->fs,
        .sync = runtime->sync,
        .preinstalled_index = "/data/lua/preinstalled.json",
        .install_root = "/data/gizclaw/lua_apps"};
    int rc = h2_gizclaw_lua_app_catalog_create(&config, &owner.apps);
    if (rc == H2_PAL_OK) {
        base_task = runtime->task;
        const h2_web_gizclaw_apps_config_t bridge = {
            .apps = owner.apps,
            .allocator = runtime->mem,
            .queue = runtime->queue,
            .task = close_fixture ? &tasks : runtime->task};
        rc = h2_web_gizclaw_apps_open(&bridge, &owner.bridge);
    }
    if (rc == H2_PAL_OK) {
        puts("H2_WEB_LUA_APPS ready");
        while (!h2_application_host_should_stop(host))
            h2_pal_time_sleep_ms(runtime->time, 10u);
    }
    int cleanup = h2_web_gizclaw_apps_close(&owner.bridge);
    if (close_fixture) {
        assert(cleanup == H2_PAL_ERR_TIMEOUT && owner.bridge != NULL);
        assert(h2_web_main_call(closed_sdk, NULL).i32 == H2_PAL_ERR_CLOSED);
        cleanup = h2_web_gizclaw_apps_close(&owner.bridge);
        assert(cleanup == H2_PAL_OK && owner.bridge == NULL);
        puts("WEB_LUA_APPS close_retry=PASS");
    }
    if (cleanup == H2_PAL_OK)
        cleanup = h2_gizclaw_lua_app_catalog_destroy(owner.apps);
    if (cleanup != H2_PAL_OK) {
        (void)h2_application_host_retain(host, &owner, cleanup);
        return cleanup;
    }
    owner.apps = NULL;
    return rc;
}

int main(void) {
    close_fixture = h2_web_main_call(fixture_mode, NULL).i32;
    inject_join_failure = close_fixture;
    static const char *const readonly[] = {"/data/lua"};
    const h2_web_application_host_config_t config = {.name = "web-lua-apps",
                                                     .persistent_root =
                                                         "/data/gizclaw",
                                                     .readonly_roots = readonly,
                                                     .readonly_root_count = 1u};
    return h2_web_application_host_run(&config, run, NULL);
}
