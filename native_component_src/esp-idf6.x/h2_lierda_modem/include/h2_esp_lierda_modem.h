#ifndef H2_ESP_LIERDA_MODEM_H
#define H2_ESP_LIERDA_MODEM_H

#include "h2_lierda_modem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_lierda_modem h2_esp_lierda_modem_t;

typedef struct h2_esp_lierda_modem_config {
    h2_lierda_modem_model_t model;
    int uart_port;
    int tx_gpio;
    int rx_gpio;
    uint32_t baud_rate; /**< Must match actual module AT UART; normally 115200.
                        * Does not reprogram the module or enable CMUX/flow control. */
    uint32_t dte_stack_size; /**< SDK DTE task bytes; 0 selects 8192. */
    uint32_t dte_priority; /**< SDK DTE priority; 0 selects 8. */
    void *power_user;
    /** Board-owned, idempotent supply/PWRKEY/settling sequence. The callback
     * returns only after the AT UART is ready, or after power-off is complete.
     * It must support closing a partial power-on failure and must not reenter
     * modem API. No GPIO polarity, PWRKEY meaning or voltage ramp is inferred. */
    h2_pal_result_t (*power)(void *user, int enabled, uint32_t timeout_ms);
    const h2_pal_mem_api_t *allocator;
    const h2_pal_sync_api_t *sync_api;
    h2_pal_modem_apn_config_t apn;
} h2_esp_lierda_modem_config_t;

/** Create a closed data-only Generic UART PPP adapter. Config/APN are copied;
 * power_user, allocator and sync_api are borrowed through successful destroy.
 * No board operation occurs in create. The caller owns exclusive UART/pins and
 * performs every control operation from ordinary task context, never the ESP
 * default event-loop callback. SKU is explicit; AT model text is not a SKU probe.
 * The ESP native SDK owns DTE/PPP tasks. IP callbacks do not call provider APIs.
 * Single-UART data mode excludes AT operations, including after lost IP or a
 * failed DATA/COMMAND transition. Successful data_close keeps module power and
 * command transport; close uses board power-off then quiesces handlers before
 * SDK resource destruction. Errors retain ownership for retry.
 * The adapter registers a MODEM_DATA netif; default-route selection remains
 * with the product's PAL netif policy and ESP-NETIF route-priority behavior.
 * No private AT, CMUX, call, GNSS, OTA, SIM hotplug or low-power behavior exists.
 * SDK config requires LWIP_PPP_SUPPORT and LWIP_IPV4; credentials additionally
 * require a compiled LWIP_PPP_PAP_SUPPORT and/or LWIP_PPP_CHAP_SUPPORT backend.
 * Missing auth backends return UNSUPPORTED, never an unauthenticated fallback.
 * On failure a non-NULL out_modem is retained only when cleanup failed; keep it
 * and dependencies alive and call destroy again. */
h2_pal_result_t h2_esp_lierda_modem_create(
    const h2_esp_lierda_modem_config_t *config,
    h2_esp_lierda_modem_t **out_modem);

/** Stop/join external callers first. Failure retains instance and SDK handlers
 * for retry; no transport callback or event-loop reentry is permitted. */
h2_pal_result_t h2_esp_lierda_modem_destroy(h2_esp_lierda_modem_t *modem);

/** Borrowed PAL object until successful destroy; NULL for a failed create. */
h2_pal_modem_api_t *h2_esp_lierda_modem_api(h2_esp_lierda_modem_t *modem);

#ifdef __cplusplus
}
#endif
#endif
