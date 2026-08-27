#include "h2_esp_simcom_teardown.h"

#include <stddef.h>

h2_pal_result_t h2_esp_simcom_run_teardown(
    const h2_esp_simcom_teardown_config_t *config) {
    if (config == NULL || config->power_off == NULL ||
        config->restore_default_netif == NULL ||
        config->unregister_event_handlers == NULL || config->destroy_dce == NULL ||
        config->destroy_netif == NULL || config->destroy_event_group == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    const h2_esp_simcom_teardown_operation_fn operations[] = {
        config->power_off,
        config->restore_default_netif,
        config->unregister_event_handlers,
        config->destroy_dce,
        config->destroy_netif,
        config->destroy_event_group,
    };
    for (size_t i = 0u; i < sizeof(operations) / sizeof(operations[0]); ++i) {
        const h2_pal_result_t rc = operations[i](config->user);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}
