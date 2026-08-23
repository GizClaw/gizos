#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_wifi_csi_get_capabilities(
    void *user,
    h2_pal_wifi_csi_capabilities_t *out_capabilities) {
    (void)user;
    if (out_capabilities != NULL) {
        out_capabilities->provider = H2_PAL_WIFI_CSI_PROVIDER_UNKNOWN;
        out_capabilities->max_sample_count = 0u;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_wifi_csi_start(
    void *user,
    const h2_pal_wifi_csi_config_t *config,
    h2_pal_wifi_csi_frame_fn frame_cb,
    void *frame_user) {
    (void)user;
    (void)config;
    (void)frame_cb;
    (void)frame_user;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_wifi_csi_stop(void *user) {
    (void)user;
    return H2_PAL_OK;
}

static const h2_pal_wifi_csi_vtable_t unsupported_wifi_csi_vtable = {
    .get_capabilities = unsupported_wifi_csi_get_capabilities,
    .start = unsupported_wifi_csi_start,
    .stop = unsupported_wifi_csi_stop,
};

static const h2_pal_wifi_csi_api_t unsupported_wifi_csi_api = {
    .user = NULL,
    .vtable = &unsupported_wifi_csi_vtable,
};

const h2_pal_wifi_csi_api_t *h2_pal_unsupported_wifi_csi_api(void) {
    return &unsupported_wifi_csi_api;
}
