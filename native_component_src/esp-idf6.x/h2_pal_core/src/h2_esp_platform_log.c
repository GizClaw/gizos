#include "h2_esp_platform_core.h"

#include "esp_log.h"

static int h2_esp_platform_log_write(void *user, h2_pal_log_level_t level, const char *scope, const char *message) {
    (void)user;

    if (message == NULL) {
        return H2_PAL_LOG_ERR_INVALID_ARG;
    }

    const char *tag = scope != NULL && scope[0] != '\0' ? scope : "h2";
    esp_log_level_t native_level;
    switch (level) {
    case H2_PAL_LOG_DEBUG:
        native_level = ESP_LOG_DEBUG;
        break;
    case H2_PAL_LOG_INFO:
        native_level = ESP_LOG_INFO;
        break;
    case H2_PAL_LOG_WARN:
        native_level = ESP_LOG_WARN;
        break;
    case H2_PAL_LOG_ERROR:
        native_level = ESP_LOG_ERROR;
        break;
    default:
        return H2_PAL_LOG_ERR_INVALID_ARG;
    }
#if ESP_LOG_VERSION == 1 && defined(CONFIG_LOG_MASTER_LEVEL) && CONFIG_LOG_MASTER_LEVEL
    if (esp_log_get_level_master() < native_level) {
        return H2_PAL_LOG_OK;
    }
#endif
    ESP_LOG_LEVEL(native_level, tag, "%s", message);
    return H2_PAL_LOG_OK;
}

const h2_pal_log_api_t *h2_esp_platform_log_api(void) {
    static const h2_pal_log_vtable_t vtable = {
        .write = h2_esp_platform_log_write,
    };
    static const h2_pal_log_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
