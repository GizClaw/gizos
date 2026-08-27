#ifndef H2_ESP_SIMCOM_TEARDOWN_H
#define H2_ESP_SIMCOM_TEARDOWN_H

#include "h2/pal/core/h2_pal_errors.h"

typedef h2_pal_result_t (*h2_esp_simcom_teardown_operation_fn)(void *user);

typedef struct h2_esp_simcom_teardown_config {
    void *user;
    h2_esp_simcom_teardown_operation_fn power_off;
    h2_esp_simcom_teardown_operation_fn restore_default_netif;
    h2_esp_simcom_teardown_operation_fn unregister_event_handlers;
    h2_esp_simcom_teardown_operation_fn destroy_dce;
    h2_esp_simcom_teardown_operation_fn destroy_netif;
    h2_esp_simcom_teardown_operation_fn destroy_event_group;
} h2_esp_simcom_teardown_config_t;

h2_pal_result_t h2_esp_simcom_run_teardown(
    const h2_esp_simcom_teardown_config_t *config);

#endif
