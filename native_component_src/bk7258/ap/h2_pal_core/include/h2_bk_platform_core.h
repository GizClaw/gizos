#ifndef H2_BK_PLATFORM_CORE_H
#define H2_BK_PLATFORM_CORE_H

#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/net/h2_pal_dtls.h"
#include "h2/pal/os/h2_pal_firmware_info.h"
#include "h2/pal/os/h2_pal_pref.h"
#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_csi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"
#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/application/h2_pal_mqtt.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_bk_task_stack_region {
    H2_BK_TASK_STACK_DEFAULT = 0,
    H2_BK_TASK_STACK_PSRAM = 1,
} h2_bk_task_stack_region_t;

typedef struct h2_bk_task_policy {
    const char *name;
    uint8_t core;
    uint8_t priority;
    uint32_t min_stack_size;
    h2_bk_task_stack_region_t stack_region;
} h2_bk_task_policy_t;

typedef bool (*h2_bk_task_policy_resolver_t)(
    void *user,
    const char *name,
    h2_bk_task_policy_t *out_policy);

h2_pal_mem_api_t *h2_bk_platform_default_allocator(void);
h2_pal_mem_api_t *h2_bk_platform_sram_allocator(void);
h2_pal_mem_api_t *h2_bk_platform_psram_allocator(void);
const h2_pal_log_api_t *h2_bk_platform_log_api(void);
const h2_pal_firmware_info_api_t *h2_bk_platform_firmware_info_api(void);
const h2_pal_net_api_t *h2_bk_platform_net_api(void);
const h2_pal_netif_api_t *h2_bk_platform_netif_api(void);
void h2_bk_platform_netif_register(
    void *netif_handle,
    h2_pal_netif_kind_t kind);
void h2_bk_platform_netif_unregister(void *netif_handle);
h2_pal_result_t h2_bk_platform_netif_monitor_init(void);
void h2_bk_platform_netif_monitor_deinit(void);
h2_pal_result_t h2_bk_platform_netif_reconcile_default(void);
h2_pal_result_t h2_bk_platform_netif_reconcile_default_async(void);
const h2_pal_sync_api_t *h2_bk_platform_sync_api(void);
const h2_pal_task_api_t *h2_bk_platform_task_api(void);
h2_pal_result_t h2_bk_platform_task_configure(
    h2_bk_task_policy_resolver_t resolver,
    void *resolver_user,
    const h2_pal_mem_api_t *allocator,
    bool reject_unknown);
const h2_pal_queue_api_t *h2_bk_platform_queue_api(void);
const h2_pal_time_api_t *h2_bk_platform_time_api(void);
const h2_pal_system_event_api_t *h2_bk_platform_system_event_api(void);
const h2_pal_crypto_api_t *h2_bk_platform_crypto_api(void);
const h2_pal_dtls_api_t *h2_bk_platform_dtls_api(void);
const h2_pal_pref_api_t *h2_bk_platform_pref_api(void);
const h2_pal_mqtt_api_t *h2_bk_platform_mqtt_api(void);
const h2_pal_webrtc_api_t *h2_bk_platform_webrtc_api(void);
h2_pal_ble_t *h2_bk_platform_ble(void);
h2_pal_wifi_sta_t *h2_bk_platform_wifi_sta(void);
h2_pal_wifi_ap_t *h2_bk_platform_wifi_ap(void);
h2_pal_wifi_settings_t *h2_bk_platform_wifi_settings(void);
const h2_pal_wifi_csi_api_t *h2_bk_platform_wifi_csi(void);
h2_pal_modem_t *h2_bk_platform_modem_unsupported(void);
const h2_pal_uart_io_stream_api_t *h2_bk_platform_uart_io_stream_api(void);
void h2_bk_platform_uart_io_stream_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
