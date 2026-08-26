#ifndef H2_ESP_PLATFORM_CORE_H
#define H2_ESP_PLATFORM_CORE_H

#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_csi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"
#include "h2/pal/net/h2_pal_dtls.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_disk.h"
#include "h2/pal/os/h2_pal_firmware_info.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_pref.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART
#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#endif
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) &&                             \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "h2/pal/hal/h2_pal_usb_jtag_io_stream.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_platform_spiffs_config {
  const char *base_path;
  const char *partition_label;
  size_t max_files;
  bool format_if_mount_failed;
} h2_esp_platform_spiffs_config_t;

typedef struct h2_esp_platform_littlefs_config {
  const char *base_path;
  const char *partition_label;
  bool format_if_mount_failed;
} h2_esp_platform_littlefs_config_t;

typedef enum h2_esp_task_core {
  H2_ESP_TASK_CORE_ANY = -1,
  H2_ESP_TASK_CORE_0 = 0,
  H2_ESP_TASK_CORE_1 = 1,
} h2_esp_task_core_t;

typedef enum h2_esp_task_stack_region {
  H2_ESP_TASK_STACK_INTERNAL = 0,
  H2_ESP_TASK_STACK_PSRAM = 1,
} h2_esp_task_stack_region_t;

typedef struct h2_esp_task_policy {
  uint32_t priority;
  h2_esp_task_core_t core;
  uint32_t min_stack_size;
  h2_esp_task_stack_region_t stack_region;
} h2_esp_task_policy_t;

typedef h2_pal_result_t (*h2_esp_task_policy_resolver_t)(
    void *user, const char *name, h2_esp_task_policy_t *out_policy);

typedef struct h2_esp_task_policy_config {
  h2_esp_task_policy_resolver_t resolver;
  h2_esp_task_policy_resolver_t fallback_resolver;
  void *resolver_user;
} h2_esp_task_policy_config_t;

h2_pal_mem_api_t *h2_esp_platform_default_allocator(void);
h2_pal_mem_api_t *h2_esp_platform_psram_allocator(void);
h2_pal_mem_api_t *h2_esp_platform_internal_allocator(void);
h2_pal_mem_api_t *h2_esp_platform_dma_allocator(void);
const h2_pal_log_api_t *h2_esp_platform_log_api(void);
const h2_pal_firmware_info_api_t *h2_esp_platform_firmware_info_api(void);
/**
 * Reads the subtype of a named data partition from an internal-stack context.
 *
 * @param label Required zero-terminated partition label.
 * @param out_subtype Caller-owned output cleared before lookup and populated
 *                    when the partition exists.
 * @return H2_PAL_OK, H2_PAL_ERR_NOT_FOUND, or a validation/execution error.
 */
h2_pal_result_t h2_esp_platform_data_partition_subtype(const char *label,
                                                       uint8_t *out_subtype);
const h2_pal_sync_api_t *h2_esp_platform_sync_api(void);
const h2_pal_net_api_t *h2_esp_platform_net_api(void);
const h2_pal_netif_api_t *h2_esp_platform_netif_api(void);
void h2_esp_platform_netif_register(void *netif_handle,
                                    h2_pal_netif_kind_t kind);
void h2_esp_platform_netif_unregister(void *netif_handle);
h2_pal_result_t h2_esp_platform_netif_monitor_init(void);
void h2_esp_platform_netif_monitor_deinit(void);
h2_pal_result_t h2_esp_platform_netif_reconcile_default(void);
const h2_pal_task_api_t *h2_esp_platform_task_api(void);
h2_pal_result_t
h2_esp_platform_task_configure(const h2_esp_task_policy_config_t *config);
const h2_pal_queue_api_t *h2_esp_platform_queue_api(void);
const h2_pal_time_api_t *h2_esp_platform_time_api(void);
const h2_pal_system_event_api_t *h2_esp_platform_system_event_api(void);
const h2_pal_crypto_api_t *h2_esp_platform_crypto_api(void);
const h2_pal_dtls_api_t *h2_esp_platform_dtls_api(void);
const h2_pal_power_api_t *h2_esp_platform_power_api(void);
h2_pal_result_t h2_esp_platform_power_before_reboot(uint32_t reason);
h2_pal_result_t h2_esp_platform_confirm_running_app(void);
h2_pal_result_t h2_esp_platform_pref_finalize_migration(void);
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART
/** @return Borrowed UART I/O stream API backed by the initialized driver. */
const h2_pal_uart_io_stream_api_t *h2_esp_platform_uart_io_stream_api(void);
#endif
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) &&                             \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
/** @return Borrowed USB Serial-JTAG I/O stream API backed by the initialized
 * driver. */
const h2_pal_usb_jtag_io_stream_api_t *
h2_esp_platform_usb_jtag_io_stream_api(void);
#endif
const h2_pal_disk_api_t *h2_esp_platform_disk_api(void);
const h2_pal_pref_api_t *h2_esp_platform_pref_api(void);
const h2_pal_webrtc_api_t *h2_esp_platform_webrtc_api(void);
h2_pal_ble_t *h2_esp_platform_ble(void);
h2_pal_wifi_sta_t *h2_esp_platform_wifi_sta(void);
h2_pal_wifi_ap_t *h2_esp_platform_wifi_ap(void);
h2_pal_wifi_settings_t *h2_esp_platform_wifi_settings(void);
const h2_pal_wifi_csi_api_t *h2_esp_platform_wifi_csi(void);
h2_pal_modem_t *h2_esp_platform_modem_unsupported(void);
int h2_esp_platform_wifi_ensure_started(void);
int h2_esp_platform_wifi_connect_saved(uint32_t timeout_ms);
int h2_esp_platform_spiffs_fs_init(
    h2_pal_fs_api_t *fs, const h2_esp_platform_spiffs_config_t *config);
int h2_esp_platform_spiffs_fs_deinit(const char *partition_label);
int h2_esp_platform_littlefs_mount(
    const h2_esp_platform_littlefs_config_t *config);
/** Formats a named LittleFS partition from an internal-stack context. */
int h2_esp_platform_littlefs_format(const char *partition_label);
int h2_esp_platform_littlefs_fs_use_base_path(h2_pal_fs_api_t *fs,
                                              const char *base_path);
int h2_esp_platform_littlefs_fs_init(
    h2_pal_fs_api_t *fs, const h2_esp_platform_littlefs_config_t *config);
int h2_esp_platform_littlefs_fs_deinit(const char *partition_label);

#ifdef __cplusplus
}
#endif

#endif
