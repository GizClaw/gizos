#include "h2_bk_platform_core.h"

#include <string.h>

#ifndef H2_BK_FIRMWARE_VERSION
#error "H2_BK_FIRMWARE_VERSION must be provided by h2_pal_core build wiring"
#endif

static h2_pal_result_t firmware_info_get_current(
    void *user,
    h2_pal_firmware_info_t *out_info) {
    static const char version[] = H2_BK_FIRMWARE_VERSION;

    (void)user;
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_info->version[0] = '\0';
    if (version[0] == '\0') {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (sizeof(version) > sizeof(out_info->version)) {
        return H2_PAL_ERR_TRUNCATED;
    }
    memcpy(out_info->version, version, sizeof(version));
    return H2_PAL_OK;
}

static const h2_pal_firmware_info_vtable_t firmware_info_vtable = {
    .get_current = firmware_info_get_current,
};

static const h2_pal_firmware_info_api_t firmware_info_api = {
    .user = NULL,
    .vtable = &firmware_info_vtable,
};

const h2_pal_firmware_info_api_t *h2_bk_platform_firmware_info_api(void) {
    return &firmware_info_api;
}
