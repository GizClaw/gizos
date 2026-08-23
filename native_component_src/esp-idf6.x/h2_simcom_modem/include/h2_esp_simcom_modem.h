#ifndef H2_ESP_SIMCOM_MODEM_H
#define H2_ESP_SIMCOM_MODEM_H

#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_simcom_modem h2_esp_simcom_modem_t;

typedef struct h2_esp_simcom_modem_config {
    int uart_port;
    int tx_gpio;
    int rx_gpio;
    int rts_gpio;
    int cts_gpio;
    int power_gpio;
    int power_on_level;
    uint32_t baud_rate;
    const char *default_apn;
    const h2_pal_sync_api_t *sync_api;
    const h2_pal_mem_api_t *allocator;
    const h2_pal_system_event_api_t *system_events;
} h2_esp_simcom_modem_config_t;

h2_pal_result_t h2_esp_simcom_modem_create(
    const h2_esp_simcom_modem_config_t *config,
    h2_esp_simcom_modem_t **out_modem);
void h2_esp_simcom_modem_destroy(h2_esp_simcom_modem_t *modem);
h2_pal_modem_api_t *h2_esp_simcom_modem_api(h2_esp_simcom_modem_t *modem);

#ifdef __cplusplus
}
#endif

#endif
