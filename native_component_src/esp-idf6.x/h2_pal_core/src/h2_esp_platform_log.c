#include "h2_esp_platform_core.h"

#include "esp_log.h"

static int h2_esp_platform_log_write(void *user, h2_pal_log_level_t level, const char *scope, const char *message) {
    (void)user;

    if (message == NULL) {
        return H2_PAL_LOG_ERR_INVALID_ARG;
    }

    const char *tag = scope != NULL && scope[0] != '\0' ? scope : "h2";
    switch (level) {
    case H2_PAL_LOG_DEBUG:
        ESP_LOGD(tag, "%s", message);
        break;
    case H2_PAL_LOG_INFO:
        ESP_LOGI(tag, "%s", message);
        break;
    case H2_PAL_LOG_WARN:
        ESP_LOGW(tag, "%s", message);
        break;
    case H2_PAL_LOG_ERROR:
        ESP_LOGE(tag, "%s", message);
        break;
    default:
        return H2_PAL_LOG_ERR_INVALID_ARG;
    }
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
