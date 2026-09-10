#include "h2_runtime_internal.h"

#include <string.h>

h2_pal_result_t h2_runtime_wifi_connect_saved(h2_runtime_t *runtime,
                                             uint32_t timeout_ms) {
    if (!h2_runtime_ready(runtime))
        return H2_PAL_ERR_INVALID_ARG;
    h2_pal_wifi_sta_config_t config = {0};
    int rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &config);
    if (rc == H2_PAL_OK)
        rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &config, timeout_ms ? timeout_ms : 15000u);
    memset(&config, 0, sizeof(config));
    return rc;
}
