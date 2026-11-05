#include "h2_pal.h"

static h2_pal_result_t unsupported_device_identity_get(
    void* user, h2_pal_device_identity_t* out_identity) {
  (void)user;
  (void)out_identity;
  return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_device_identity_vtable_t
    unsupported_device_identity_vtable = {
        .get = unsupported_device_identity_get,
};
static const h2_pal_device_identity_api_t unsupported_device_identity_api = {
    .user = NULL,
    .vtable = &unsupported_device_identity_vtable,
};

const h2_pal_device_identity_api_t* h2_pal_unsupported_device_identity_api(
    void) {
  return &unsupported_device_identity_api;
}
