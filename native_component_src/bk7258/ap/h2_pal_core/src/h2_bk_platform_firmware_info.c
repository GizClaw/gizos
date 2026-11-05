#include "h2_bk_platform_core.h"

#include "components/system.h"

#include <string.h>

#ifndef H2_BK_FIRMWARE_VERSION
#error "H2_BK_FIRMWARE_VERSION must be provided by h2_pal_core build wiring"
#endif

static h2_pal_result_t firmware_info_get_current(
    void* user, h2_pal_firmware_info_t* out_info) {
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

const h2_pal_firmware_info_api_t* h2_bk_platform_firmware_info_api(void) {
  return &firmware_info_api;
}

static h2_pal_result_t device_identity_get(
    void* user, h2_pal_device_identity_t* out_identity) {
  (void)user;
  if (out_identity == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_identity, 0, sizeof(*out_identity));
  uint8_t mac[6];
  if (bk_get_mac(mac, MAC_TYPE_BLUETOOTH) != BK_OK) return H2_PAL_ERR_IO;
  memcpy(out_identity->bytes, mac, sizeof(mac));
  out_identity->size = sizeof(mac);
  return H2_PAL_OK;
}

const h2_pal_device_identity_api_t* h2_bk_platform_device_identity_api(void) {
  static const h2_pal_device_identity_vtable_t vtable = {
      .get = device_identity_get,
  };
  static const h2_pal_device_identity_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
