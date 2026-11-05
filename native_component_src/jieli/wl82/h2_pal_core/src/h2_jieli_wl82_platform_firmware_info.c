#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

#include <string.h>

/* The native build passes the Bazel firmware version as a string macro; the
 * fallback keeps host tests and ad-hoc SDK builds honest about its origin. */
#ifndef H2_JIELI_FIRMWARE_VERSION
#define H2_JIELI_FIRMWARE_VERSION "jieli-wl82-dev"
#endif

static h2_pal_result_t firmware_info_get_current(
    void* user, h2_pal_firmware_info_t* out_info) {
  static const char version[] = H2_JIELI_FIRMWARE_VERSION;
  (void)user;
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (sizeof(version) > sizeof(out_info->version)) {
    out_info->version[0] = '\0';
    return H2_PAL_ERR_TRUNCATED;
  }
  memcpy(out_info->version, version, sizeof(version));
  return H2_PAL_OK;
}

static const h2_pal_firmware_info_vtable_t s_firmware_info_vtable = {
    .get_current = firmware_info_get_current,
};

static const h2_pal_firmware_info_api_t s_firmware_info_api = {
    .user = NULL,
    .vtable = &s_firmware_info_vtable,
};

const h2_pal_firmware_info_api_t* h2_jieli_wl82_platform_firmware_info_api(
    void) {
  return &s_firmware_info_api;
}

static h2_pal_result_t device_identity_get(
    void* user, h2_pal_device_identity_t* out_identity) {
  (void)user;
  if (out_identity == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_identity, 0, sizeof(*out_identity));
  uint8_t identity[6];
  h2_pal_result_t result = h2_jieli_sdk_device_identity(identity);
  if (result != H2_PAL_OK) return result;
  memcpy(out_identity->bytes, identity, sizeof(identity));
  out_identity->size = sizeof(identity);
  return H2_PAL_OK;
}

const h2_pal_device_identity_api_t* h2_jieli_wl82_platform_device_identity_api(
    void) {
  static const h2_pal_device_identity_vtable_t vtable = {
      .get = device_identity_get,
  };
  static const h2_pal_device_identity_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
