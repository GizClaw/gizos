#ifndef H2_BK_PLATFORM_CORE_H
#define H2_BK_PLATFORM_CORE_H

#include "application/h2_pal_mqtt.h"
#include "application/h2_pal_webrtc.h"
#include "hal/h2_pal_ble.h"
#include "hal/h2_pal_modem.h"
#include "hal/h2_pal_uart_io_stream.h"
#include "hal/h2_pal_wifi.h"
#include "hal/h2_pal_wifi_csi.h"
#include "hal/h2_pal_wifi_settings.h"
#include "net/h2_pal_dtls.h"
#include "net/h2_pal_net.h"
#include "net/h2_pal_netif.h"
#include "os/h2_pal_crypto.h"
#include "os/h2_pal_device_identity.h"
#include "os/h2_pal_firmware_info.h"
#include "os/h2_pal_io_stream.h"
#include "os/h2_pal_log.h"
#include "os/h2_pal_mem.h"
#include "os/h2_pal_pref.h"
#include "os/h2_pal_queue.h"
#include "os/h2_pal_sync.h"
#include "os/h2_pal_system_event.h"
#include "os/h2_pal_task.h"
#include "os/h2_pal_time.h"
#include "os/h2_pal_timer.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_bk_task_stack_region {
  H2_BK_TASK_STACK_DEFAULT = 0,
  H2_BK_TASK_STACK_PSRAM = 1,
} h2_bk_task_stack_region_t;

/** Native unpinned SDK affinity, preserving rtos_create_thread semantics. */
#define H2_BK_TASK_CORE_ANY UINT32_MAX

typedef struct h2_bk_task_policy {
  const char* sdk_name;
  uint32_t core;
  uint32_t priority;
  uint32_t min_stack_size;
  h2_bk_task_stack_region_t stack_region;
} h2_bk_task_policy_t;

typedef h2_pal_result_t (*h2_bk_task_policy_resolver_t)(
    void* user, const char* name, h2_bk_task_policy_t* out_policy);

typedef struct h2_bk_task_policy_config {
  h2_bk_task_policy_resolver_t resolver;
  void* resolver_user;
  const h2_pal_mem_api_t* task_allocator;
  /** Optional actual PSRAM stack allocator. Borrowed until every task joins.
   * Generated AP policies supply this so the native TCB follows task_allocator
   * while the stack follows the per-task PSRAM policy. */
  const h2_pal_mem_api_t* psram_stack_allocator;
} h2_bk_task_policy_config_t;

typedef struct h2_bk_platform_resource_stats {
  size_t live_tasks, task_stack_bytes;
  size_t live_queues, live_mutexes, live_semaphores, live_conditions;
  size_t live_timers, live_firmware_infos;
  size_t allocations, allocation_bytes;
} h2_bk_platform_resource_stats_t;
/** Actual live native PAL objects and allocator usable bytes. Task stack
 * reservations remain owned until join. Compare snapshots at quiescence. */
h2_pal_result_t h2_bk_platform_get_resource_stats(
    h2_bk_platform_resource_stats_t* out);
const h2_pal_timer_api_t* h2_bk_platform_timer_api(void);

h2_pal_mem_api_t* h2_bk_platform_default_allocator(void);
h2_pal_mem_api_t* h2_bk_platform_sram_allocator(void);
h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void);
const h2_pal_log_api_t* h2_bk_platform_log_api(void);
const h2_pal_firmware_info_api_t* h2_bk_platform_firmware_info_api(void);
/** Local Bluetooth MAC in the existing Loader UID byte order. */
const h2_pal_device_identity_api_t* h2_bk_platform_device_identity_api(void);
const h2_pal_net_api_t* h2_bk_platform_net_api(void);
const h2_pal_netif_api_t* h2_bk_platform_netif_api(void);
void h2_bk_platform_netif_register(void* netif_handle,
                                   h2_pal_netif_kind_t kind);
void h2_bk_platform_netif_unregister(void* netif_handle);
h2_pal_result_t h2_bk_platform_netif_monitor_init(void);
void h2_bk_platform_netif_monitor_deinit(void);
h2_pal_result_t h2_bk_platform_netif_reconcile_default(void);
h2_pal_result_t h2_bk_platform_netif_reconcile_default_async(void);
const h2_pal_sync_api_t* h2_bk_platform_sync_api(void);
const h2_pal_task_api_t* h2_bk_platform_task_api(void);
h2_pal_result_t h2_bk_platform_task_configure(
    const h2_bk_task_policy_config_t* config);
/** Resolve and validate a native task's generated target policy, sealing Task
 * configuration even when lookup fails. Call from serialized startup task
 * context after task_configure, never from an ISR. name is borrowed for this
 * call and passed unchanged to the resolver (including NULL for unnamed PAL
 * tasks). out_policy is required, caller-owned and cleared on failure. Any
 * sdk_name in the result is borrowed from the configured resolver and must
 * remain alive until native creation returns. No task or stack is allocated.
 * Returns INVALID_ARG, INVALID_STATE, NOT_FOUND, TASK, or OK. */
h2_pal_result_t h2_bk_platform_task_reserve_policy(
    const char* name, h2_bk_task_policy_t* out_policy);
const h2_pal_queue_api_t* h2_bk_platform_queue_api(void);
const h2_pal_time_api_t* h2_bk_platform_time_api(void);
const h2_pal_system_event_api_t* h2_bk_platform_system_event_api(void);
const h2_pal_crypto_api_t* h2_bk_platform_crypto_api(void);
const h2_pal_dtls_api_t* h2_bk_platform_dtls_api(void);
const h2_pal_pref_api_t* h2_bk_platform_pref_api(void);
const h2_pal_mqtt_api_t* h2_bk_platform_mqtt_api(void);
const h2_pal_webrtc_api_t* h2_bk_platform_webrtc_api(void);
h2_pal_ble_t* h2_bk_platform_ble(void);
h2_pal_wifi_sta_t* h2_bk_platform_wifi_sta(void);
h2_pal_wifi_ap_t* h2_bk_platform_wifi_ap(void);
h2_pal_wifi_settings_t* h2_bk_platform_wifi_settings(void);
const h2_pal_wifi_csi_api_t* h2_bk_platform_wifi_csi(void);
h2_pal_modem_t* h2_bk_platform_modem_unsupported(void);
const h2_pal_io_stream_api_t* h2_bk_platform_io_stream_api(void);
const h2_pal_uart_io_stream_api_t* h2_bk_platform_uart_io_stream_api(void);
void h2_bk_platform_uart_io_stream_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
