#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_wifi_settings_get_saved_sta_config(void *p0, h2_pal_wifi_sta_config_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_settings_set_saved_sta_config(void *p0, const h2_pal_wifi_sta_config_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_settings_clear_saved_sta_config(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_settings_has_saved_sta_config(void *p0, int *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_settings_list(void *user,
                                          h2_pal_wifi_saved_network_t *out,
                                          size_t capacity, size_t *out_count) {
  (void)user;
  (void)out;
  (void)capacity;
  if (out_count)
    *out_count = 0;
  return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_settings_remove(void *user, const char *ssid,
                                            size_t ssid_len) {
  (void)user;
  (void)ssid;
  (void)ssid_len;
  return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_wifi_settings_vtable_t unsupported_wifi_settings_vtable = {
    .get_saved_sta_config = unsupported_wifi_settings_get_saved_sta_config,
    .set_saved_sta_config = unsupported_wifi_settings_set_saved_sta_config,
    .clear_saved_sta_config = unsupported_wifi_settings_clear_saved_sta_config,
    .has_saved_sta_config = unsupported_wifi_settings_has_saved_sta_config,
    .list_saved_sta_configs = unsupported_wifi_settings_list,
    .remove_saved_sta_config = unsupported_wifi_settings_remove,
};
static const h2_pal_wifi_settings_api_t unsupported_wifi_settings_api = { .user = NULL, .vtable = &unsupported_wifi_settings_vtable };
const h2_pal_wifi_settings_api_t *h2_pal_unsupported_wifi_settings_api(void) { return &unsupported_wifi_settings_api; }
