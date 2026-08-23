#include "h2_bk3633_platform_core.h"

#include <string.h>

#ifndef H2_BK3633_FIRMWARE_VERSION
#error "H2_BK3633_FIRMWARE_VERSION must be provided by build wiring"
#endif

int main(void)
{
    h2_pal_firmware_info_t info = {
        .version = "stale",
    };

    if (h2_pal_firmware_info_get_current(
            h2_bk3633_platform_firmware_info_api(),
            &info) != H2_PAL_OK) {
        return 1;
    }
    if (strcmp(info.version, H2_BK3633_FIRMWARE_VERSION) != 0) {
        return 2;
    }
    if (h2_pal_firmware_info_get_current(
            h2_bk3633_platform_firmware_info_api(),
            NULL) != H2_PAL_ERR_INVALID_ARG) {
        return 3;
    }
    return 0;
}
