#include "h2_lua_apps.h"
#include "h2_app_test_fs.h"
#include "h2_desktop_platform.h"
#include "fixtures.h"
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
  h2_lua_apps_config_t config = {.allocator = &memory,
                                 .fs = &fs.api,
                                 .sync = h2_desktop_platform_sync_api(),
                                 .install_root = "/apps"};
  h2_lua_apps_t *apps = NULL;
  assert(h2_lua_apps_create(&config, &apps) == H2_PAL_OK);
  h2_lua_app_install_t *install = NULL;
  h2_lua_app_info_t info;
  assert(h2_lua_app_install_begin(apps, sizeof(fixture_valid),
                                  fixture_valid_sha256, NULL, NULL,
                                  &install) == 0);
  assert(h2_lua_app_install_write(install, fixture_valid,
                                  sizeof(fixture_valid)) == 0);
  assert(h2_lua_app_install_finish(&install, &info) == 0 && install == NULL);
  fs.close = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1};
  assert(h2_lua_app_install_begin(apps, sizeof(fixture_update), NULL, NULL,
                                  NULL, &install) == 0);
  assert(h2_lua_app_install_write(install, fixture_update,
                                  sizeof(fixture_update)) == H2_PAL_ERR_IO);
  assert(h2_lua_app_install_finish(&install, &info) == H2_PAL_ERR_IO &&
         install == NULL);
  assert(h2_lua_apps_get(apps, "demo", &info) == 0 &&
         !strcmp(info.version, "1.0.0"));
  assert(fs.active_handles == 1u);
  assert(h2_lua_app_install_begin(apps, sizeof(fixture_valid), NULL, NULL, NULL,
                                  &install) == H2_PAL_ERR_IO &&
         install == NULL);
  assert(h2_lua_apps_destroy(apps) == H2_PAL_ERR_IO && fs.active_handles == 1u);
  assert(h2_app_test_fs_deinit(&fs) == H2_PAL_ERR_INVALID_STATE);
  /* The PAL error does not prove handle consumption. This process owns the
   * retained manager/provider and exits as its explicit recovery boundary;
   * neither retrying an ambiguous close nor freeing under a live file is safe.
   */
  return 0;
}
