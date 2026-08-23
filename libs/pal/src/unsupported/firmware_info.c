#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_firmware_info_get_current(
    void *user,
    h2_pal_firmware_info_t *out_info) {
    (void)user;
    (void)out_info;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_firmware_info_vtable_t unsupported_firmware_info_vtable = {
    .get_current = unsupported_firmware_info_get_current,
};
static const h2_pal_firmware_info_api_t unsupported_firmware_info_api = {
    .user = NULL,
    .vtable = &unsupported_firmware_info_vtable,
};
const h2_pal_firmware_info_api_t *h2_pal_unsupported_firmware_info_api(void) {
    return &unsupported_firmware_info_api;
}
