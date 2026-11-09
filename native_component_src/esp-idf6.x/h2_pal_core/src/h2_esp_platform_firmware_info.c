#include "h2_esp_platform_core.h"

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#include <string.h>

static h2_pal_result_t firmware_info_get_current(
    void* user, h2_pal_firmware_info_t* out_info) {
  (void)user;
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_info->version[0] = '\0';

  const esp_app_desc_t* description = esp_app_get_description();
  if (description == NULL || description->version[0] == '\0') {
    return H2_PAL_ERR_INVALID_STATE;
  }
  const char* end =
      memchr(description->version, '\0', sizeof(description->version));
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

const h2_pal_firmware_info_api_t* h2_esp_platform_firmware_info_api(void) {
  return &firmware_info_api;
}

static h2_pal_result_t device_identity_get(
    void* user, h2_pal_device_identity_t* out_identity) {
  (void)user;
  if (out_identity == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_identity, 0, sizeof(*out_identity));
  uint8_t mac[6];
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4
  /* P4 has no local Bluetooth MAC. Never query the hosted C6 identity. */
  const esp_err_t result = esp_efuse_mac_get_default(mac);
#else
  const esp_err_t result = esp_read_mac(mac, ESP_MAC_BT);
#endif
  if (result != ESP_OK) return H2_PAL_ERR_IO;
  memcpy(out_identity->bytes, mac, sizeof(mac));
  out_identity->size = sizeof(mac);
  return H2_PAL_OK;
}

const h2_pal_device_identity_api_t* h2_esp_platform_device_identity_api(void) {
  static const h2_pal_device_identity_vtable_t vtable = {
      .get = device_identity_get,
  };
  static const h2_pal_device_identity_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
