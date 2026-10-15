#include "fixtures.h"
#include "h2_app_test_fs.h"
#include "h2_app_test_sync.h"
#include "h2_desktop_platform.h"
#include "h2_gizclaw_lua_app_catalog.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void *allocate(void *user, size_t size) {
    (void)user;
    return malloc(size);
}
static void *resize(void *user, void *ptr, size_t size) {
    (void)user;
    return realloc(ptr, size);
}
static void release(void *user, void *ptr) {
    (void)user;
    free(ptr);
}
static const h2_pal_mem_vtable_t memory_vtable = {allocate, resize, release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};

int main(void) {
    h2_app_test_fs_t fs;
    assert(h2_app_test_fs_init(&fs, &memory) == H2_PAL_OK);
    h2_gizclaw_lua_app_catalog_config_t config = {
        .allocator = &memory,
        .fs = &fs.api,
        .sync = h2_desktop_platform_sync_api(),
        .install_root = "/apps"};
    h2_app_test_sync_t partial_sync;
    h2_app_test_sync_init(&partial_sync);
    h2_gizclaw_lua_app_catalog_config_t partial = config;
    partial.sync = &partial_sync.api;
    partial.preinstalled_index = "/missing";
    fs.stat = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1};
    partial_sync.destroy =
        (h2_app_test_fault_t){.result = H2_PAL_ERR_BUSY, .remaining = 1};
    h2_gizclaw_lua_app_catalog_t *retained = NULL;
    assert(h2_gizclaw_lua_app_catalog_create(&partial, &retained) ==
           H2_PAL_ERR_BUSY);
    assert(retained != NULL && partial_sync.mutexes[0].active);
    assert(h2_gizclaw_lua_app_catalog_destroy(retained) == H2_PAL_OK);
    assert(!partial_sync.mutexes[0].active);
    h2_gizclaw_lua_app_catalog_t *apps = NULL;
    assert(h2_gizclaw_lua_app_catalog_create(&config, &apps) == H2_PAL_OK);
    h2_gizclaw_lua_app_install_t *install = NULL;
    h2_gizclaw_lua_app_info_t info;
    assert(h2_gizclaw_lua_app_install_begin(apps, sizeof(fixture_valid),
                                            fixture_valid_sha256, NULL, NULL,
                                            &install) == 0);
    assert(h2_gizclaw_lua_app_install_write(install, fixture_valid,
                                            sizeof(fixture_valid)) == 0);
    assert(h2_gizclaw_lua_app_install_finish(&install, &info) == 0 &&
           install == NULL);
    fs.close = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1};
    assert(h2_gizclaw_lua_app_install_begin(apps, sizeof(fixture_update), NULL,
                                            NULL, NULL, &install) == 0);
    assert(h2_gizclaw_lua_app_install_write(install, fixture_update,
                                            sizeof(fixture_update)) ==
           H2_PAL_ERR_IO);
    assert(h2_gizclaw_lua_app_install_finish(&install, &info) ==
               H2_PAL_ERR_IO &&
           install == NULL);
    assert(h2_gizclaw_lua_app_catalog_get(apps, "demo", &info) == 0 &&
           !strcmp(info.version, "1.0.0"));
    assert(fs.active_handles == 1u);
    assert(h2_gizclaw_lua_app_install_begin(apps, sizeof(fixture_valid), NULL,
                                            NULL, NULL,
                                            &install) == H2_PAL_ERR_IO &&
           install == NULL);
    assert(h2_gizclaw_lua_app_catalog_destroy(apps) == H2_PAL_ERR_IO &&
           fs.active_handles == 1u);
    assert(h2_app_test_fs_deinit(&fs) == H2_PAL_ERR_INVALID_STATE);
    /* The PAL error does not prove handle consumption. This process owns the
     * retained manager/provider and exits as its explicit recovery boundary;
     * neither retrying an ambiguous close nor freeing under a live file is
     * safe.
     */
    return 0;
}
