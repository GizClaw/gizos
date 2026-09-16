#include "h2_esp_platform_core.h"

static int h2_esp_wifi_settings_unsupported_get_saved(
    void *user,
    h2_pal_wifi_sta_config_t *out_config) {
    (void)user;
    (void)out_config;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_settings_unsupported_set_saved(
    void *user,
    const h2_pal_wifi_sta_config_t *config) {
    (void)user;
    (void)config;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_settings_unsupported_clear_saved(void *user) {
    (void)user;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_settings_unsupported_has_saved(
    void *user,
    int *out_has_config) {
    (void)user;
    if (out_has_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_has_config = 0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_wifi_settings_vtable_t s_h2_esp_wifi_settings_vtable = {
    .get_saved_sta_config = h2_esp_wifi_settings_unsupported_get_saved,
    .set_saved_sta_config = h2_esp_wifi_settings_unsupported_set_saved,
    .clear_saved_sta_config = h2_esp_wifi_settings_unsupported_clear_saved,
    .has_saved_sta_config = h2_esp_wifi_settings_unsupported_has_saved,
};

static h2_pal_wifi_settings_t s_h2_esp_wifi_settings = {
    .user = NULL,
    .vtable = &s_h2_esp_wifi_settings_vtable,
};

h2_pal_wifi_settings_t *h2_esp_platform_wifi_settings(void) {
    return &s_h2_esp_wifi_settings;
}
