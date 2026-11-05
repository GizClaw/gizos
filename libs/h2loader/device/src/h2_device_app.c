#include "h2_device_service.h"
#include "h2_device_app_preflight.h"
#include "h2loader_app_task_names.h"

#include <stdio.h>
#include <string.h>

static uint64_t app_now_ms(void* user) {
  uint64_t now = 0u;
  (void)h2_pal_time_get_monotonic_ms(user, &now);
  return now;
}
static void app_sleep_ms(void* user, uint32_t ms) {
  (void)h2_pal_time_sleep_ms(user, ms);
}
static void app_wifi_activity(void* user, bool active) {
  h2_device_app_commands_t* self = user;
  if (self->service != NULL)
    (void)h2_loader_ble_service_request_advertising_paused(self->service,
                                                           active);
}
static int same_text(const char* a, const char* b) {
  return a == b || (a != NULL && b != NULL && strcmp(a, b) == 0);
}
static int config_matches(const h2_device_app_commands_t* self,
                          const h2_device_app_commands_config_t* config) {
  const h2_device_app_commands_config_t* a = &self->config;
  return (config->active_version == NULL ||
          same_text(a->active_version, config->active_version)) &&
         same_text(a->app_entry_path, config->app_entry_path) &&
         a->command_allocator == config->command_allocator &&
         a->hardware_capabilities == config->hardware_capabilities &&
         a->h2loader_partition_id == config->h2loader_partition_id &&
         a->app_partition_id == config->app_partition_id &&
         a->coredump_partition_id == config->coredump_partition_id &&
         a->boot_policy_flags == config->boot_policy_flags &&
         a->identity_policy == config->identity_policy &&
         a->digest_yield_ms == config->digest_yield_ms &&
         a->serial_admission_fallback_ms ==
             config->serial_admission_fallback_ms &&
         a->serial_post_write_poll_ms == config->serial_post_write_poll_ms &&
         a->command_stack_size == config->command_stack_size;
}

int h2_device_app_commands_prepare(
    h2_device_app_commands_t* self, const h2_runtime_config_t* runtime,
    const h2_device_app_commands_config_t* config) {
  if (self == NULL || runtime == NULL || config == NULL ||
      runtime->task == NULL || runtime->sync == NULL || runtime->mem == NULL ||
      runtime->pref == NULL || runtime->power == NULL ||
      runtime->image == NULL || runtime->firmware_info == NULL ||
      runtime->time == NULL || runtime->fs == NULL ||
      runtime->device_identity == NULL || runtime->board == NULL ||
      runtime->target == NULL || runtime->chip == NULL ||
      config->active_name == NULL || config->active_name[0] == '\0' ||
      config->h2loader_partition_id == 0u ||
      (config->hardware_capabilities & ~H2_LOADER_CAPABILITIES_ALL) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (self->prepared) {
    return config_matches(self, config) &&
                   self->client_config.pref == runtime->pref &&
                   self->boot.config.image == runtime->image &&
                   self->boot.config.power == runtime->power &&
                   self->client_config.operation_sync == runtime->sync
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  }
  if (self->operation_mutex != NULL) return H2_PAL_ERR_INVALID_STATE;
  h2_device_app_preflight_t preflight;
  int rc = h2_device_app_preflight(runtime, &preflight);
  if (rc != H2_PAL_OK) return rc;
  self->operation_mutex = preflight.operation_mutex;
  if (config->app_partition_id != 0u &&
      config->app_partition_id != preflight.app_partition_id) {
    rc = H2_PAL_ERR_INVALID_STATE;
    goto cleanup;
  }
  rc = h2_loader_pal_device_uid(runtime->device_identity, self->device_uid);
  if (rc != H2_PAL_OK) goto cleanup;
  const h2_device_boot_policy_config_t boot = {
      .image = runtime->image,
      .power = runtime->power,
      .time = runtime->time,
      .allocator = config->command_allocator != NULL
                       ? config->command_allocator
                       : (runtime->mem_external != NULL ? runtime->mem_external
                                                        : runtime->mem),
      .app_entry_path = config->app_entry_path,
      .fs = runtime->fs,
      .pref = runtime->pref,
      .loader_partition_id = config->h2loader_partition_id,
      .app_partition_id = preflight.app_partition_id,
      .digest_yield_ms = config->digest_yield_ms,
      .flags = config->boot_policy_flags,
      .app_identity = config->identity_policy,
  };
  rc = h2_device_boot_policy_init(&self->boot, &boot);
  if (rc != H2_PAL_OK) goto cleanup;
  self->client_config = (h2_loader_app_client_config_t){
      .pref = runtime->pref,
      .power = &self->boot.power,
      .allocator = boot.allocator,
      .disk = runtime->disk,
      .fs = runtime->fs,
      .http = runtime->http,
      .wifi = runtime->wifi_sta,
      .wifi_settings = runtime->wifi_settings,
      .app_entry_path = config->app_entry_path,
      .digest = self->boot.pal.digest,
      .operation_sync = runtime->sync,
      .operation_mutex = self->operation_mutex,
      .wifi_operation_sync = runtime->sync,
      .wifi_operation_mutex = self->operation_mutex,
      .board = runtime->board,
      .target = runtime->target,
      .chip = runtime->chip,
      .device_uid = self->device_uid,
      .hardware_capabilities = config->hardware_capabilities,
      .h2loader_partition_id = config->h2loader_partition_id,
      .app_partition_id = preflight.app_partition_id,
      .coredump_partition_id = config->coredump_partition_id,
      .memory_stats = {.user = (void*)runtime->mem,
                       .read = h2_loader_pal_memory_stats_read},
      .clock_user = (void*)runtime->time,
      .now_ms = app_now_ms,
      .sleep_ms = app_sleep_ms,
  };
  rc = h2_device_boot_policy_current_identity(
      &self->boot, H2_LOADER_IMAGE_ROLE_APP, runtime->board, runtime->target,
      config->active_version != NULL && config->active_version[0] != '\0'
          ? config->active_version
          : preflight.firmware_info.version,
      &self->client_config.active_identity);
  if (rc == H2_PAL_OK)
    rc = h2_loader_app_client_init(&self->serial_client, &self->client_config);
  if (rc != H2_PAL_OK) goto cleanup;
  self->config = *config;
  self->task = runtime->task;
  self->io_stream = runtime->io_stream;
  self->prepared = 1;
  return H2_PAL_OK;
cleanup: {
  int cleaned = h2_pal_mutex_destroy(runtime->sync, self->operation_mutex);
  if (cleaned != H2_PAL_OK) return cleaned;
  self->operation_mutex = NULL;
  memset(&self->client_config, 0, sizeof(self->client_config));
  memset(&self->serial_client, 0, sizeof(self->serial_client));
  return rc;
}
}

int h2_device_app_commands_prepare_runtime(
    h2_device_app_commands_t* self, h2_runtime_t* runtime,
    const h2_device_app_commands_config_t* config) {
  if (runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  const h2_runtime_config_t preinit = {
      .task = runtime->task,
      .sync = runtime->sync,
      .mem = runtime->mem,
      .mem_external = runtime->mem_external,
      .io_stream = runtime->io_stream,
      .pref = runtime->pref,
      .power = runtime->power,
      .image = runtime->image,
      .firmware_info = runtime->firmware_info,
      .time = runtime->time,
      .fs = runtime->fs,
      .disk = runtime->disk,
      .http = runtime->http,
      .wifi_sta = runtime->wifi_sta,
      .wifi_settings = runtime->wifi_settings,
      .device_identity = runtime->device_identity,
      .board = runtime->board,
      .target = runtime->target,
      .chip = runtime->chip,
  };
  return h2_device_app_commands_prepare(self, &preinit, config);
}

int h2_device_app_commands_prepare_serial(
    h2_device_app_commands_t* self, const h2_runtime_config_t* runtime,
    const h2_device_app_commands_config_t* config,
    const h2_iostreamikcp_io_t* physical_io) {
  if (self == NULL || config == NULL ||
      (config->hardware_capabilities & H2_LOADER_CAPABILITY_UART) == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (self->serial_started) return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_device_app_commands_prepare(self, runtime, config);
  if (rc != H2_PAL_OK) return rc;
  return h2_device_app_commands_start_serial(self, physical_io);
}
int h2_device_app_commands_start_serial(
    h2_device_app_commands_t* self, const h2_iostreamikcp_io_t* physical_io) {
  if (self == NULL || !self->prepared) return H2_PAL_ERR_INVALID_ARG;
  if (self->serial_started) return H2_PAL_ERR_INVALID_STATE;
  h2_iostreamikcp_io_t public_io;
  if (physical_io == NULL) {
    int rc = h2_pal_io_stream_start(self->io_stream);
    if (rc != H2_PAL_OK) return rc;
    self->io_stream_started = 1;
    public_io = h2_iostreamikcp_io_from_stream(self->io_stream);
    physical_io = &public_io;
  }
  const h2_device_app_commands_config_t* config = &self->config;
  const h2_device_app_serial_config_t serial = {
      .transport = {.physical_io = *physical_io,
                    .allocator = self->client_config.allocator,
                    .time = self->boot.config.time,
                    .admission_fallback_ms =
                        config->serial_admission_fallback_ms,
                    .post_write_poll_ms = config->serial_post_write_poll_ms},
      .task = self->task,
      .stack_size = config->command_stack_size != 0u
                        ? config->command_stack_size
                        : H2_DEVICE_COMMAND_STACK_SIZE,
  };
  int rc = h2_device_app_serial_start_with_config(
      &self->serial, &self->serial_client, &serial);
  if (rc == H2_PAL_OK) {
    self->serial_started = 1;
  } else if (self->io_stream_started) {
    int released = h2_pal_io_stream_release(self->io_stream);
    if (released == H2_PAL_OK)
      self->io_stream_started = 0;
    else
      rc = released;
  }
  return rc;
}

static int handle_ble_session(void* user, h2_bleikcp_t* stream,
                              uint16_t conn_handle) {
  h2_device_app_commands_t* self = user;
  h2_loader_app_client_t client;
  (void)conn_handle;
  if (self == NULL || self->runtime == NULL || stream == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_loader_app_client_init(&client, &self->client_config);
  if (rc != H2_PAL_OK) return rc;
  const h2_loader_app_client_return_console_config_t console = {
      .client = &client,
      .task = self->runtime->task,
      .read_user = stream,
      .read_byte = h2_loader_ble_app_read_byte,
      .write_user = stream,
      .write = h2_loader_ble_app_write,
      .task_name = h2loader_app_command_task_name,
      .stack_size = self->config.command_stack_size != 0u
                        ? self->config.command_stack_size
                        : H2_DEVICE_COMMAND_STACK_SIZE,
  };
  rc = h2_loader_app_client_run_return_console(&console);
  h2_loader_deinit(&client.loader);
  return rc;
}

int h2_device_app_commands_start(
    h2_device_app_commands_t* self, h2_runtime_t* runtime,
    const h2_device_app_commands_config_t* config) {
  if (self == NULL || runtime == NULL || runtime->ble_host == NULL ||
      runtime->task == NULL || runtime->time == NULL || runtime->sync == NULL ||
      runtime->system_event == NULL || runtime->mem == NULL || config == NULL ||
      (config->hardware_capabilities & H2_LOADER_CAPABILITY_BLE) == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!self->prepared || self->service != NULL) return H2_PAL_ERR_INVALID_STATE;
  if (!config_matches(self, config)) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_bleikcp_global_init();
  if (rc != H2_PAL_OK) return rc;
  self->runtime = runtime;
  const h2_loader_ble_service_config_t service = {
      .local_name = runtime->ble_local_name != NULL ? runtime->ble_local_name
                                                    : config->ble_local_name,
      .log = runtime->log,
      .api = {.ble = runtime->ble_host,
              .task = runtime->task,
              .time = runtime->time,
              .sync = runtime->sync,
              .system_event = runtime->system_event,
              .allocator = runtime->mem},
      .board = self->client_config.board,
      .capabilities = self->client_config.hardware_capabilities,
      .advertising_mode = config->ble_advertising_mode,
      .handler = handle_ble_session,
      .handler_user = self,
      .command_stack_size = config->command_stack_size != 0u
                                ? config->command_stack_size
                                : H2_DEVICE_COMMAND_STACK_SIZE,
  };
  uint32_t attempts =
      config->ble_start_attempts != 0u ? config->ble_start_attempts : 6u;
  uint32_t retry_ms = 250u;
  for (uint32_t attempt = 1u; attempt <= attempts; ++attempt) {
    rc = h2_loader_ble_service_open(&service, &self->service);
    if (rc == H2_PAL_OK) {
      (void)h2_pal_wifi_sta_set_activity_observer(runtime->wifi_sta,
                                                  app_wifi_activity, self);
      return H2_PAL_OK;
    }
    self->service = NULL;
    if (attempt == attempts) break;
    printf("H2_DEVICE_APP_BLE_START_RETRY rc=%d attempt=%u delay_ms=%u\n", rc,
           (unsigned)attempt, (unsigned)retry_ms);
    rc = h2_pal_time_sleep_ms(runtime->time, retry_ms);
    if (rc != H2_PAL_OK) break;
    retry_ms = retry_ms < 2500u ? retry_ms * 2u : 5000u;
  }
  self->runtime = NULL;
  (void)h2_bleikcp_global_shutdown();
  return rc;
}
int h2_device_app_operation_lock(h2_device_app_commands_t* self) {
  return self != NULL && self->prepared
             ? h2_pal_mutex_lock(self->client_config.operation_sync,
                                 self->operation_mutex)
             : H2_PAL_ERR_INVALID_STATE;
}
int h2_device_app_operation_unlock(h2_device_app_commands_t* self) {
  return self != NULL && self->prepared
             ? h2_pal_mutex_unlock(self->client_config.operation_sync,
                                   self->operation_mutex)
             : H2_PAL_ERR_INVALID_STATE;
}
int h2_device_app_confirm(h2_device_app_commands_t* self,
                          h2_runtime_t* runtime) {
  if (self == NULL || runtime == NULL || runtime->firmware_info == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!self->prepared) {
    int rc =
        h2_device_app_commands_prepare_runtime(self, runtime, &self->config);
    if (rc != H2_PAL_OK) return rc;
  }
  int rc = h2_device_app_operation_lock(self);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_firmware_info_t firmware;
  h2_loader_image_identity_t identity;
  rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &firmware);
  if (rc == H2_PAL_OK)
    rc = h2_device_boot_policy_current_identity(
        &self->boot, H2_LOADER_IMAGE_ROLE_APP, self->client_config.board,
        self->client_config.target, firmware.version, &identity);
  if (rc == H2_PAL_OK)
    rc = h2_loader_finalize_active_app_with_confirmation(
        self->client_config.pref, self->client_config.allocator,
        self->client_config.fs, H2_LOADER_DEFAULT_PACKAGE_PATH, &identity,
        self->boot.running_partition_id, self->client_config.app_partition_id,
        h2_device_boot_policy_confirm_app, &self->boot);
  if (rc == H2_PAL_OK)
    rc = h2_pal_pref_finalize_migration(self->client_config.pref);
  int unlocked = h2_device_app_operation_unlock(self);
  if (rc == H2_PAL_OK) {
    rc = unlocked;
    h2_device_app_commands_admit_sessions(self);
  }
  return rc;
}
int h2_device_app_commands_stop_serial(h2_device_app_commands_t* self) {
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_device_app_serial_stop(&self->serial);
  if (rc != H2_PAL_OK) return rc;
  self->serial_started = 0;
  if (self->io_stream_started) {
    rc = h2_pal_io_stream_release(self->io_stream);
    if (rc == H2_PAL_OK) self->io_stream_started = 0;
  }
  return rc;
}
void h2_device_app_commands_admit_sessions(h2_device_app_commands_t* self) {
  if (self != NULL) h2_device_app_serial_admit_sessions(&self->serial);
}
int h2_device_app_commands_pause_ble_advertising(
    h2_device_app_commands_t* self) {
  return self != NULL && self->service != NULL
             ? h2_loader_ble_service_pause_advertising(self->service)
             : H2_PAL_ERR_INVALID_STATE;
}
int h2_device_app_commands_resume_ble_advertising(
    h2_device_app_commands_t* self) {
  return self != NULL && self->service != NULL
             ? h2_loader_ble_service_resume_advertising(self->service)
             : H2_PAL_ERR_INVALID_STATE;
}
int h2_device_app_commands_advertise_ble_service(
    h2_device_app_commands_t* self, const h2_pal_ble_uuid_t* uuid) {
  return self != NULL && self->service != NULL && uuid != NULL
             ? h2_loader_ble_service_set_additional_advertised_services(
                   self->service, uuid, 1u)
             : H2_PAL_ERR_INVALID_STATE;
}
int h2_device_app_commands_get_config(h2_device_app_commands_t* self,
                                      h2_loader_app_client_config_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (self == NULL || !self->prepared) return H2_PAL_ERR_INVALID_STATE;
  *out = self->client_config;
  return H2_PAL_OK;
}

int h2_device_app_commands_prepare_named(h2_device_app_commands_t* self,
                                         const h2_runtime_config_t* runtime,
                                         const char* active_name,
                                         uint32_t loader_partition_id,
                                         uint32_t coredump_partition_id) {
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_device_app_commands_config_t config = self->config;
  config.active_name = active_name;
  config.h2loader_partition_id = loader_partition_id;
  config.coredump_partition_id = coredump_partition_id;
  return h2_device_app_commands_prepare_serial(self, runtime, &config, NULL);
}

int h2_device_app_commands_start_named(h2_device_app_commands_t* self,
                                       h2_runtime_t* runtime,
                                       const char* active_name,
                                       uint32_t loader_partition_id,
                                       uint32_t coredump_partition_id) {
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_device_app_commands_config_t config = self->config;
  config.active_name = active_name;
  config.h2loader_partition_id = loader_partition_id;
  config.coredump_partition_id = coredump_partition_id;
  int rc = self->prepared
               ? H2_PAL_OK
               : h2_device_app_commands_prepare_runtime(self, runtime, &config);
  return rc == H2_PAL_OK ? h2_device_app_commands_start(self, runtime, &config)
                         : rc;
}

int h2_device_app_commands_start_serial_named(h2_device_app_commands_t* self,
                                              h2_runtime_t* runtime,
                                              const char* active_name,
                                              uint32_t hardware_capabilities) {
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_device_app_commands_config_t config = self->config;
  config.active_name = active_name;
  config.hardware_capabilities = hardware_capabilities;
  int rc = h2_device_app_commands_prepare_runtime(self, runtime, &config);
  return rc == H2_PAL_OK ? h2_device_app_commands_start_serial(self, NULL) : rc;
}

int h2_device_app_current_identity(h2_device_app_commands_t* self,
                                   h2_runtime_t* runtime,
                                   h2_loader_image_role_t role,
                                   const char* board, const char* target,
                                   const char* version,
                                   h2_loader_image_identity_t* out_identity) {
  if (out_identity == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_identity, 0, sizeof(*out_identity));
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = self->prepared ? H2_PAL_OK
                          : h2_device_app_commands_prepare_runtime(
                                self, runtime, &self->config);
  return rc == H2_PAL_OK
             ? h2_device_boot_policy_current_identity(
                   &self->boot, role, board, target, version, out_identity)
             : rc;
}

h2_loader_digest_api_t h2_device_app_commands_digest(
    const h2_device_app_commands_t* self) {
  return self != NULL && self->prepared ? self->boot.pal.digest
                                        : (h2_loader_digest_api_t){0};
}

const h2_pal_power_api_t* h2_device_app_commands_power(
    const h2_device_app_commands_t* self) {
  return self != NULL && self->prepared ? &self->boot.power : NULL;
}
