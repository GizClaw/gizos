#ifndef H2_ESP_WIFI_TEARDOWN_H
#define H2_ESP_WIFI_TEARDOWN_H

typedef int (*h2_esp_wifi_teardown_operation_fn)(void *user);
typedef int (*h2_esp_wifi_teardown_map_error_fn)(void *user, int error);

typedef struct h2_esp_wifi_teardown_config {
    void *user;
    h2_esp_wifi_teardown_operation_fn dhcp_stop;
    h2_esp_wifi_teardown_operation_fn wifi_stop;
    h2_esp_wifi_teardown_operation_fn wifi_deinit;
    h2_esp_wifi_teardown_map_error_fn map_error;
    int success;
    int dhcp_already_stopped;
    int wifi_not_initialized;
    int wifi_not_started;
} h2_esp_wifi_teardown_config_t;

int h2_esp_wifi_run_driver_teardown(const h2_esp_wifi_teardown_config_t *config);

#endif
