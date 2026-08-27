#include "h2_esp_wifi_teardown.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>

int h2_esp_wifi_run_driver_teardown(const h2_esp_wifi_teardown_config_t *config) {
    if (config == NULL || config->dhcp_stop == NULL || config->wifi_stop == NULL ||
        config->wifi_deinit == NULL || config->map_error == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    int result = config->dhcp_stop(config->user);
    if (result != config->success && result != config->dhcp_already_stopped) {
        return config->map_error(config->user, result);
    }

    result = config->wifi_stop(config->user);
    if (result != config->success && result != config->wifi_not_initialized &&
        result != config->wifi_not_started) {
        return config->map_error(config->user, result);
    }

    result = config->wifi_deinit(config->user);
    if (result != config->success && result != config->wifi_not_initialized) {
        return config->map_error(config->user, result);
    }
    return H2_PAL_OK;
}
