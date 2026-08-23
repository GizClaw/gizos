#include "h2_esp_platform_core.h"

#include "esp_app_desc.h"

#include <string.h>

static h2_pal_result_t firmware_info_get_current(
    void *user,
    h2_pal_firmware_info_t *out_info) {
    (void)user;
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_info->version[0] = '\0';

    const esp_app_desc_t *description = esp_app_get_description();
    if (description == NULL || description->version[0] == '\0') {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const char *end = memchr(
        description->version,
        '\0',
        sizeof(description->version));
    if (end == NULL) {
        return H2_PAL_ERR_TRUNCATED;
    }
    size_t version_len = (size_t)(end - description->version);
    if (version_len >= sizeof(out_info->version)) {
        return H2_PAL_ERR_TRUNCATED;
    }
    memcpy(out_info->version, description->version, version_len + 1u);
    return H2_PAL_OK;
}

static const h2_pal_firmware_info_vtable_t firmware_info_vtable = {
    .get_current = firmware_info_get_current,
};

static const h2_pal_firmware_info_api_t firmware_info_api = {
    .user = NULL,
    .vtable = &firmware_info_vtable,
};

const h2_pal_firmware_info_api_t *h2_esp_platform_firmware_info_api(void) {
    return &firmware_info_api;
}
