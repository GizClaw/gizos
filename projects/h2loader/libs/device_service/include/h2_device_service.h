#ifndef H2_DEVICE_SERVICE_H
#define H2_DEVICE_SERVICE_H

#include "h2_device_boot_policy.h"
#include "h2_device_activity.h"
#include "h2_device_serial.h"
#include "h2_loader_ble.h"
#include "h2_runtime.h"
#include "h2loader_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_DEVICE_COMMAND_STACK_SIZE (32u * 1024u)
#define H2_DEVICE_APP_ADMISSION_FALLBACK_MS 8000u

typedef struct h2_device_loader_config {
  const char* board;
  const char* app_entry_path;
  const h2_pal_mem_api_t* command_allocator;
  uint32_t loader_partition_id;
  uint32_t app_partition_id;
  uint32_t coredump_partition_id;
  uint32_t hardware_capabilities;
  uint32_t boot_policy_flags;
  h2_device_identity_policy_t identity_policy;
  uint32_t digest_yield_ms;
  uint32_t serial_idle_timeout_ms;
  uint32_t serial_post_write_poll_ms;
  h2_loader_ble_advertising_mode_t ble_advertising_mode;
  int force_command_mode;
  uint32_t mfg_required_total;
  void* user;
  h2_loader_mfg_command_fn mfg_command;
  int (*run_mfg)(void* user, h2_loader_t* loader,
                 const h2_pal_sync_api_t* operation_sync,
                 h2_pal_mutex_t* operation_mutex,
                 const h2_pal_sync_api_t* wifi_operation_sync,
                 h2_pal_mutex_t* wifi_operation_mutex);
  void (*rearm_mfg)(void* user);
  int (*before_disruptive)(void* user, h2_loader_disruptive_action_t action);
  void (*install_progress)(void* user, h2_loader_install_phase_t phase,
                           uint64_t completed, uint64_t total,
                           const char* detail);
  void (*show_installing)(void* user);
  void (*show_install_failed)(void* user, int code);
  void (*show_launching)(void* user);
  /** Product diagnostics after startup, before accepting command sessions. */
  void (*command_ready)(void* user, h2_loader_t* loader);
  /** Product policy may extend UI/MFG configuration before services start. */
  int (*extend_app)(h2_runtime_t* runtime, h2loader_app_config_t* config);
  const h2loader_app_command_service_api_t* command_service;
} h2_device_loader_config_t;

/** Caller-owned state, borrowed public APIs retained through blocking run. */
typedef struct h2_device_loader {
  h2_runtime_t* runtime;
  const h2_device_loader_config_t* config;
  h2_device_boot_policy_t boot;
  h2_device_serial_t serial;
  h2_atomic_bool_t stop_requested;
  h2_loader_command_config_t ble_command;
  h2_loader_ble_service_t* ble_service;
  h2_device_activity_t wifi_activity;
} h2_device_loader_t;

int h2_device_loader_run(h2_device_loader_t* state, h2_runtime_t* runtime,
                         const h2_device_loader_config_t* config,
                         const h2_iostreamikcp_io_t* physical_io, int with_ble);

typedef struct h2_device_app_commands_config {
  const char* active_name;
  const char* active_version;
  uint32_t hardware_capabilities;
  uint32_t h2loader_partition_id;
  uint32_t app_partition_id;
  uint32_t coredump_partition_id;
  const char* app_entry_path;
  const char* ble_local_name;
  const h2_pal_mem_api_t* command_allocator;
  uint32_t boot_policy_flags;
  h2_device_identity_policy_t identity_policy;
  uint32_t digest_yield_ms;
  uint32_t serial_admission_fallback_ms;
  uint32_t serial_post_write_poll_ms;
  size_t command_stack_size;
  /** Zero selects six attempts and 250/500/1000/2000/4000ms waits. */
  uint32_t ble_start_attempts;
  h2_loader_ble_advertising_mode_t ble_advertising_mode;
} h2_device_app_commands_config_t;

/** Boot-lifetime, zero-initialized composition. Config prepares serial before
 * Runtime; BLE attaches later. Every client, Stage, digest, reboot and App
 * confirmation borrows the same recursive operation mutex. */
typedef struct h2_device_app_commands {
  h2_runtime_t* runtime;
  const h2_pal_task_api_t* task;
  const h2_pal_io_stream_api_t* io_stream;
  h2_device_boot_policy_t boot;
  h2_loader_app_client_t serial_client;
  h2_device_app_serial_t serial;
  h2_loader_app_client_config_t client_config;
  h2_device_app_commands_config_t config;
  h2_loader_ble_service_t* service;
  h2_pal_mutex_t* operation_mutex;
  char device_uid[H2_LOADER_DEVICE_UID_MAX];
  int prepared;
  int serial_started;
  int io_stream_started;
} h2_device_app_commands_t;

/** Prepare without publishing a transport, also usable for BLE-only products.
 */
int h2_device_app_commands_prepare(
    h2_device_app_commands_t* state, const h2_runtime_config_t* runtime_config,
    const h2_device_app_commands_config_t* config);
int h2_device_app_commands_prepare_runtime(
    h2_device_app_commands_t* state, h2_runtime_t* runtime,
    const h2_device_app_commands_config_t* config);
int h2_device_app_commands_start_serial(
    h2_device_app_commands_t* state, const h2_iostreamikcp_io_t* physical_io);
int h2_device_app_commands_prepare_serial(
    h2_device_app_commands_t* state, const h2_runtime_config_t* runtime_config,
    const h2_device_app_commands_config_t* config,
    const h2_iostreamikcp_io_t* physical_io);
int h2_device_app_commands_start(h2_device_app_commands_t* state,
                                 h2_runtime_t* runtime,
                                 const h2_device_app_commands_config_t* config);
int h2_device_app_commands_stop_serial(h2_device_app_commands_t* state);
void h2_device_app_commands_admit_sessions(h2_device_app_commands_t* state);
int h2_device_app_operation_lock(h2_device_app_commands_t* state);
int h2_device_app_operation_unlock(h2_device_app_commands_t* state);
int h2_device_app_confirm(h2_device_app_commands_t* state,
                          h2_runtime_t* runtime);
int h2_device_app_commands_pause_ble_advertising(
    h2_device_app_commands_t* state);
int h2_device_app_commands_resume_ble_advertising(
    h2_device_app_commands_t* state);
int h2_device_app_commands_advertise_ble_service(
    h2_device_app_commands_t* state, const h2_pal_ble_uuid_t* service_uuid);
int h2_device_app_commands_get_config(
    h2_device_app_commands_t* state, h2_loader_app_client_config_t* out_config);

/** Named entry helpers retain caller-owned configuration and use only the
 * Runtime's public PAL stream. A NULL physical_io on the lower-level entry
 * points also selects that stream. Native providers own its physical setup. */
int h2_device_app_commands_prepare_named(h2_device_app_commands_t* state,
                                         const h2_runtime_config_t* runtime,
                                         const char* active_name,
                                         uint32_t loader_partition_id,
                                         uint32_t coredump_partition_id);
int h2_device_app_commands_start_named(h2_device_app_commands_t* state,
                                       h2_runtime_t* runtime,
                                       const char* active_name,
                                       uint32_t loader_partition_id,
                                       uint32_t coredump_partition_id);
int h2_device_app_commands_start_serial_named(h2_device_app_commands_t* state,
                                              h2_runtime_t* runtime,
                                              const char* active_name,
                                              uint32_t hardware_capabilities);
int h2_device_app_current_identity(h2_device_app_commands_t* state,
                                   h2_runtime_t* runtime,
                                   h2_loader_image_role_t role,
                                   const char* board, const char* target,
                                   const char* version,
                                   h2_loader_image_identity_t* out_identity);
h2_loader_digest_api_t h2_device_app_commands_digest(
    const h2_device_app_commands_t* state);
const h2_pal_power_api_t* h2_device_app_commands_power(
    const h2_device_app_commands_t* state);

#ifdef __cplusplus
}
#endif
#endif
