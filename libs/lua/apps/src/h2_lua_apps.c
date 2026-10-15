#include "h2_lua_apps.h"

h2_pal_result_t h2_lua_apps_create(const h2_lua_apps_config_t *config,
                                   h2_lua_apps_t **out) {
  return h2_gizclaw_lua_app_catalog_create(config, out);
}
h2_pal_result_t h2_lua_apps_destroy(h2_lua_apps_t *apps) {
  return h2_gizclaw_lua_app_catalog_destroy(apps);
}
h2_pal_result_t h2_lua_apps_list(h2_lua_apps_t *apps, h2_lua_app_info_t *out,
                                 size_t capacity, size_t *count) {
  return h2_gizclaw_lua_app_catalog_list(apps, out, capacity, count);
}
h2_pal_result_t h2_lua_apps_get(h2_lua_apps_t *apps, const char *id,
                                h2_lua_app_info_t *out) {
  return h2_gizclaw_lua_app_catalog_get(apps, id, out);
}
bool h2_lua_apps_can_install(const h2_lua_apps_t *apps) {
  return h2_gizclaw_lua_app_catalog_can_install(apps);
}
bool h2_lua_app_id_valid(const char *id) {
  return h2_gizclaw_lua_app_id_valid(id);
}
h2_pal_result_t h2_lua_apps_uninstall(h2_lua_apps_t *apps, const char *id) {
  return h2_gizclaw_lua_app_catalog_uninstall(apps, id);
}
h2_pal_result_t h2_lua_app_install_begin(h2_lua_apps_t *apps, uint64_t size,
                                         const char *sha,
                                         h2_pal_http_cancel_fn cancel,
                                         void *user,
                                         h2_lua_app_install_t **out) {
  return h2_gizclaw_lua_app_install_begin(apps, size, sha, cancel, user, out);
}
h2_pal_result_t h2_lua_app_install_write(h2_lua_app_install_t *install,
                                         const uint8_t *bytes, size_t size) {
  return h2_gizclaw_lua_app_install_write(install, bytes, size);
}
h2_pal_result_t h2_lua_app_install_finish(h2_lua_app_install_t **install,
                                          h2_lua_app_info_t *out) {
  return h2_gizclaw_lua_app_install_finish(install, out);
}
void h2_lua_app_install_abort(h2_lua_app_install_t **install) {
  h2_gizclaw_lua_app_install_abort(install);
}
h2_pal_result_t h2_lua_apps_install(h2_lua_apps_t *apps,
                                    const h2_pal_http_api_t *http,
                                    h2_lua_app_source_t source, const char *sha,
                                    uint32_t timeout,
                                    h2_pal_http_cancel_fn cancel, void *user,
                                    h2_lua_app_info_t *out) {
  return h2_gizclaw_lua_app_catalog_install(apps, http, source, sha, timeout,
                                            cancel, user, out);
}
