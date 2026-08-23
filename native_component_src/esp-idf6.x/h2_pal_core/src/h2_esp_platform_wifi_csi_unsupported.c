#include "h2_esp_platform_core.h"

static h2_pal_result_t h2_esp_wifi_csi_unsupported_capabilities(
    void *user, h2_pal_wifi_csi_capabilities_t *out_capabilities) {
  (void)user;
  out_capabilities->provider = H2_PAL_WIFI_CSI_PROVIDER_ESP_IDF;
  out_capabilities->max_sample_count = 0u;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_wifi_csi_unsupported_start(
    void *user, const h2_pal_wifi_csi_config_t *config,
    h2_pal_wifi_csi_frame_fn frame_cb, void *frame_user) {
  (void)user;
  (void)config;
  (void)frame_cb;
  (void)frame_user;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_wifi_csi_unsupported_stop(void *user) {
  (void)user;
  return H2_PAL_OK;
}

static const h2_pal_wifi_csi_vtable_t s_h2_esp_wifi_csi_unsupported_vtable = {
    .get_capabilities = h2_esp_wifi_csi_unsupported_capabilities,
    .start = h2_esp_wifi_csi_unsupported_start,
    .stop = h2_esp_wifi_csi_unsupported_stop,
};

static const h2_pal_wifi_csi_api_t s_h2_esp_wifi_csi_unsupported = {
    .user = NULL,
    .vtable = &s_h2_esp_wifi_csi_unsupported_vtable,
};

const h2_pal_wifi_csi_api_t *h2_esp_platform_wifi_csi(void) {
  return &s_h2_esp_wifi_csi_unsupported;
}
