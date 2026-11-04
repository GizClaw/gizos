#include "h2_device_service.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the real App client, serial startup, identity hashing and finalizer.
 * Only external PAL capabilities and the BLE service boundary are fixtures. */
static unsigned creates, destroys, task_starts, ble_opens, shutdowns;
static unsigned open_failures, delay_count, pauses, allocations, confirmations;
static unsigned commits, lock_depth, digest_yields, native_reads, stored_reads;
static uint32_t delays[8];
static uint64_t clock_ms;
static int sleep_failure, task_failure, join_failure;
static int stream_start_failure, stream_release_failure;
static unsigned stream_starts, stream_releases;
static h2_loader_ble_service_config_t ble_config;
static const uint8_t image_bytes[] = "native-image-with-package-padding";

static void* allocate(void* user, size_t size) {
  (void)user;
  ++allocations;
  return malloc(size);
}
static void* reallocate(void* user, void* pointer, size_t size) {
  (void)user;
  if (pointer == NULL) ++allocations;
  return realloc(pointer, size);
}
static void free_memory(void* user, void* pointer) {
  (void)user;
  if (pointer != NULL) {
    assert(allocations);
    --allocations;
    free(pointer);
  }
}
static int create_mutex(void* user, const h2_pal_mutex_config_t* config,
                        h2_pal_mutex_t** out) {
  (void)user;
  assert(config->flags == H2_PAL_MUTEX_FLAG_RECURSIVE);
  assert(!strcmp(config->name, "h2loader-app-operation"));
  ++creates;
  *out = (h2_pal_mutex_t*)&creates;
  return 0;
}
static int destroy_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex == (void*)&creates);
  ++destroys;
  return 0;
}
static int lock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex == (void*)&creates);
  ++lock_depth;
  return 0;
}
static int unlock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex == (void*)&creates && lock_depth);
  --lock_depth;
  return 0;
}
static int get_firmware(void* user, h2_pal_firmware_info_t* out) {
  (void)user;
  strcpy(out->version, "1.2.3");
  return 0;
}
static int get_running(void* user, h2_pal_power_boot_partition_t* out) {
  (void)user;
  *out = (h2_pal_power_boot_partition_t){
      .id = 2u, .flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_APP};
  return 0;
}
static int confirm_image(void* user,
                         const h2_pal_power_native_evidence_t* evidence,
                         h2_pal_power_image_confirmation_t* out) {
  (void)user;
  assert(evidence == NULL && lock_depth == 2u && commits != 0u);
  ++confirmations;
  *out = (h2_pal_power_image_confirmation_t){.partition_id = 2u};
  return 0;
}
static int get_time(void* user, uint64_t* out) {
  (void)user;
  *out = clock_ms;
  return 0;
}
static int sleep_ms(void* user, uint32_t ms) {
  (void)user;
  clock_ms += ms;
  if (ms == 1u) {
    ++digest_yields;
    return 0;
  }
  assert(delay_count < 8u);
  delays[delay_count++] = ms;
  return sleep_failure ? H2_PAL_ERR_TASK : 0;
}
static int get_identity(void* user, h2_pal_device_identity_t* out) {
  (void)user;
  *out =
      (h2_pal_device_identity_t){.bytes = {0, 17, 34, 51, 68, 85}, .size = 6u};
  return 0;
}
static int image_info(void* user, uint32_t id, h2_pal_image_view_t view,
                      h2_pal_image_info_t* out) {
  (void)user;
  assert(id == 2u);
  *out = (h2_pal_image_info_t){
      .capacity = sizeof(image_bytes),
      .image_size =
          view == H2_PAL_IMAGE_VIEW_NATIVE ? 12u : sizeof(image_bytes),
      .flags = H2_PAL_IMAGE_FLAG_READABLE | H2_PAL_IMAGE_FLAG_SIZE_KNOWN};
  return 0;
}
static int image_read(void* user, uint32_t id, h2_pal_image_view_t view,
                      uint64_t offset, void* out, size_t len) {
  (void)user;
  assert(id == 2u && offset + len <= sizeof(image_bytes));
  if (view == H2_PAL_IMAGE_VIEW_NATIVE)
    ++native_reads;
  else
    ++stored_reads;
  memcpy(out, image_bytes + offset, len);
  return 0;
}
static int ns_close(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  return 0;
}
static int ns_commit(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  assert(lock_depth);
  ++commits;
  return 0;
}
static int set_blob(h2_pal_pref_namespace_t* ns, const char* key,
                    const void* blob, size_t len) {
  (void)ns;
  assert(key && blob && len && lock_depth);
  return 0;
}
static int pref_open(void* user, const char* name, h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)user;
  assert(!strcmp(name, H2_LOADER_PREF_NAMESPACE));
  if (mode == H2_PAL_PREF_OPEN_READ_ONLY) return H2_PAL_ERR_NOT_FOUND;
  static h2_pal_pref_namespace_t ns = {
      .close = ns_close, .commit = ns_commit, .set_blob = set_blob};
  *out = &ns;
  return 0;
}
static int start_task(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* context,
                      h2_pal_task_t** out) {
  (void)user;
  assert(options->min_stack_size == 32768u && entry && context);
  ++task_starts;
  if (task_failure) return H2_PAL_ERR_TASK;
  *out = (h2_pal_task_t*)&task_starts;
  return 0;
}
static int join_task(void* user, h2_pal_task_t* task) {
  (void)user;
  assert(task != NULL);
  return join_failure ? H2_PAL_ERR_TIMEOUT : 0;
}
static int observe_wifi(void* user, h2_pal_wifi_activity_fn callback,
                        void* context) {
  (void)user;
  callback(context, true);
  return 0;
}
static int physical_read(void* user, void* data, size_t len, size_t* out,
                         uint32_t timeout) {
  (void)user;
  (void)data;
  (void)len;
  *out = 0u;
  clock_ms += timeout;
  return H2_PAL_ERR_TIMEOUT;
}
static int physical_write(void* user, const void* data, size_t len, size_t* out,
                          uint32_t timeout) {
  (void)user;
  (void)data;
  (void)timeout;
  *out = len;
  return 0;
}
static int start_stream(void* user) {
  (void)user;
  ++stream_starts;
  return stream_start_failure ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static int release_stream(void* user) {
  (void)user;
  ++stream_releases;
  return stream_release_failure ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static int flush_stream(void* user) {
  (void)user;
  return H2_PAL_OK;
}

int h2_bleikcp_global_init(void) { return 0; }
int h2_bleikcp_global_shutdown(void) {
  ++shutdowns;
  return 0;
}
int h2_loader_ble_service_open(const h2_loader_ble_service_config_t* config,
                               h2_loader_ble_service_t** out) {
  assert(task_starts);
  ble_config = *config;
  ++ble_opens;
  *out = NULL;
  if (ble_opens <= open_failures) return H2_PAL_ERR_IO;
  *out = (h2_loader_ble_service_t*)&ble_opens;
  return 0;
}
int h2_loader_ble_service_request_advertising_paused(
    h2_loader_ble_service_t* service, bool paused) {
  assert(service == (void*)&ble_opens && paused);
  ++pauses;
  return 0;
}
int h2_loader_ble_service_pause_advertising(h2_loader_ble_service_t* service) {
  (void)service;
  return 0;
}
int h2_loader_ble_service_resume_advertising(h2_loader_ble_service_t* service) {
  (void)service;
  return 0;
}
int h2_loader_ble_service_set_additional_advertised_services(
    h2_loader_ble_service_t* service, const h2_pal_ble_uuid_t* uuid,
    size_t count) {
  (void)service;
  (void)uuid;
  assert(count == 1u);
  return 0;
}
int h2_loader_ble_app_read_byte(void* user, uint32_t timeout) {
  (void)user;
  (void)timeout;
  return H2_LOADER_APP_CLIENT_SESSION_CLOSED;
}
int h2_loader_ble_app_write(void* user, const char* data, size_t len) {
  (void)user;
  (void)data;
  (void)len;
  return 0;
}

int main(void) {
  const h2_pal_mem_vtable_t mem_vt = {
      .alloc = allocate, .realloc = reallocate, .free = free_memory};
  const h2_pal_mem_api_t mem = {.vtable = &mem_vt};
  const h2_pal_sync_vtable_t sync_vt = {.create_mutex = create_mutex,
                                        .destroy_mutex = destroy_mutex,
                                        .lock_mutex = lock_mutex,
                                        .unlock_mutex = unlock_mutex};
  const h2_pal_sync_api_t sync = {.vtable = &sync_vt};
  const h2_pal_firmware_info_vtable_t firmware_vt = {.get_current =
                                                         get_firmware};
  const h2_pal_firmware_info_api_t firmware = {.vtable = &firmware_vt};
  const h2_pal_power_vtable_t power_vt = {
      .get_running_boot_partition = get_running,
      .confirm_running_image = confirm_image};
  const h2_pal_power_api_t power = {.vtable = &power_vt};
  const h2_pal_time_vtable_t time_vt = {.sleep_ms = sleep_ms,
                                        .get_monotonic_ms = get_time};
  const h2_pal_time_api_t time = {.vtable = &time_vt};
  const h2_pal_device_identity_vtable_t identity_vt = {.get = get_identity};
  const h2_pal_device_identity_api_t identity = {.vtable = &identity_vt};
  const h2_pal_image_vtable_t image_vt = {.get_info = image_info,
                                          .read = image_read};
  const h2_pal_image_api_t image = {.vtable = &image_vt};
  const h2_pal_pref_vtable_t pref_vt = {.open = pref_open};
  const h2_pal_pref_api_t pref = {.vtable = &pref_vt};
  const h2_pal_task_vtable_t task_vt = {.start = start_task, .join = join_task};
  const h2_pal_task_api_t task = {.vtable = &task_vt};
  const h2_pal_wifi_sta_vtable_t wifi_vt = {.set_activity_observer =
                                                observe_wifi};
  const h2_pal_wifi_sta_api_t wifi = {.vtable = &wifi_vt};
  const h2_pal_fs_api_t fs = {0};
  const h2_pal_disk_api_t disk = {0};
  const h2_pal_http_api_t http = {0};
  const h2_pal_ble_api_t ble = {0};
  const h2_pal_system_event_api_t event = {0};
  const h2_runtime_config_t preinit = {.task = &task,
                                       .mem = &mem,
                                       .sync = &sync,
                                       .pref = &pref,
                                       .power = &power,
                                       .firmware_info = &firmware,
                                       .time = &time,
                                       .fs = &fs,
                                       .disk = &disk,
                                       .http = &http,
                                       .wifi_sta = &wifi,
                                       .image = &image,
                                       .device_identity = &identity,
                                       .board = "board",
                                       .target = "target",
                                       .chip = "chip"};
  h2_device_app_commands_t state = {0};
  const h2_device_app_commands_config_t commands = {
      .active_name = "app",
      .hardware_capabilities = 7u,
      .h2loader_partition_id = 1u,
      .coredump_partition_id = 3u,
      .ble_local_name = "fallback",
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u};
  const h2_iostreamikcp_io_t physical = {.read = physical_read,
                                         .write = physical_write};
  assert(h2_device_app_commands_prepare_serial(&state, &preinit, &commands,
                                               &physical) == 0);
  assert(state.serial_started && state.prepared && task_starts == 1u &&
         creates == 1u && !destroys);
  assert(native_reads == 1u && stored_reads == 0u && digest_yields == 1u);
  assert(state.client_config.active_identity.image_size == 12u);
  assert(!strcmp(state.device_uid, "001122334455"));
  assert(h2_device_app_commands_prepare_serial(&state, &preinit, &commands,
                                               &physical) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_loader_app_client_config_t copied;
  assert(h2_device_app_commands_get_config(&state, &copied) == 0);
  assert(copied.operation_mutex == state.operation_mutex &&
         copied.digest.user == state.client_config.digest.user);
  assert(copied.wifi_operation_mutex == copied.operation_mutex);
  h2_runtime_t runtime = {.ble_host = &ble,
                          .task = &task,
                          .time = &time,
                          .sync = &sync,
                          .system_event = &event,
                          .mem = &mem,
                          .wifi_sta = &wifi,
                          .board = "board",
                          .ble_local_name = "canonical",
                          .firmware_info = &firmware};
  open_failures = 6u;
  assert(h2_device_app_commands_start(&state, &runtime, &commands) ==
         H2_PAL_ERR_IO);
  const uint32_t expected[] = {250u, 500u, 1000u, 2000u, 4000u};
  assert(ble_opens == 6u && delay_count == 5u && shutdowns == 1u);
  assert(!memcmp(delays, expected, sizeof(expected)));
  assert(state.serial_started && !state.runtime && !state.service && !destroys);
  ble_opens = delay_count = 0u;
  open_failures = 1u;
  sleep_failure = 1;
  assert(h2_device_app_commands_start(&state, &runtime, &commands) ==
         H2_PAL_ERR_TASK);
  assert(state.serial_started && ble_opens == 1u && shutdowns == 2u);
  ble_opens = delay_count = 0u;
  open_failures = 5u;
  sleep_failure = 0;
  assert(h2_device_app_commands_start(&state, &runtime, &commands) == 0);
  assert(ble_opens == 6u && pauses == 1u &&
         !strcmp(ble_config.local_name, "canonical"));
  unsigned before = allocations;
  assert(ble_config.handler(ble_config.handler_user, (h2_bleikcp_t*)&state,
                            1u) == 0);
  assert(allocations ==
         before); /* Synchronous BLE cannot retain a stack client. */
  assert(h2_device_app_operation_lock(&state) == 0);
  assert(h2_device_app_confirm(&state, &runtime) == 0 && confirmations == 1u &&
         lock_depth == 1u);
  assert(h2_device_app_operation_unlock(&state) == 0);
  assert(
      h2_atomic_bool_load(&state.serial.sessions_admitted, H2_ATOMIC_ACQUIRE));
  assert(h2_device_app_commands_stop_serial(&state) == 0 &&
         !state.serial_started);
  assert(h2_device_app_commands_start_serial(&state, &physical) == 0);
  assert(h2_device_app_commands_stop_serial(&state) == 0);
  h2_loader_deinit(&state.serial_client.loader);
  assert(allocations == 0u && !lock_depth);

  const h2_pal_io_stream_vtable_t stream_vt = {
      .start = start_stream,
      .read = physical_read,
      .write = physical_write,
      .flush = flush_stream,
      .release = release_stream,
  };
  const h2_pal_io_stream_api_t stream = {.vtable = &stream_vt};
  const h2_pal_mem_api_t external = {.vtable = &mem_vt};
  h2_runtime_config_t stream_config = preinit;
  stream_config.io_stream = &stream;
  stream_config.mem_external = &external;
  h2_device_app_commands_t managed = {.config = commands};
  task_failure = 1;
  assert(h2_device_app_commands_prepare_named(
             &managed, &stream_config, "managed", 1u, 3u) == H2_PAL_ERR_TASK);
  assert(managed.prepared && !managed.serial_started &&
         !managed.io_stream_started && stream_starts == 1u &&
         stream_releases == 1u);
  assert(managed.client_config.allocator == &external);
  task_failure = 0;
  stream_start_failure = 1;
  assert(h2_device_app_commands_start_serial(&managed, NULL) == H2_PAL_ERR_IO);
  assert(stream_starts == 2u && stream_releases == 1u);
  stream_start_failure = 0;
  assert(h2_device_app_commands_start_serial(&managed, NULL) == H2_PAL_OK);
  assert(managed.serial_started && managed.io_stream_started);
  const h2_iostreamikcp_io_t adapted = h2_iostreamikcp_io_from_stream(&stream);
  size_t written = 0u;
  assert(adapted.write(adapted.user, image_bytes, sizeof(image_bytes), &written,
                       50u) == H2_PAL_OK &&
         written == sizeof(image_bytes));
  assert(adapted.flush(adapted.user) == H2_PAL_OK);
  join_failure = 1;
  assert(h2_device_app_commands_stop_serial(&managed) == H2_PAL_ERR_TIMEOUT);
  assert(managed.serial_started && managed.io_stream_started &&
         stream_releases == 1u);
  join_failure = 0;
  stream_release_failure = 1;
  assert(h2_device_app_commands_stop_serial(&managed) == H2_PAL_ERR_IO);
  assert(!managed.serial_started && managed.io_stream_started &&
         stream_releases == 2u);
  stream_release_failure = 0;
  assert(h2_device_app_commands_stop_serial(&managed) == H2_PAL_OK);
  assert(!managed.io_stream_started && stream_releases == 3u);
  h2_loader_deinit(&managed.serial_client.loader);
  h2_device_app_commands_t explicit_allocator = {0};
  h2_device_app_commands_config_t explicit_config = commands;
  explicit_config.command_allocator = &mem;
  assert(h2_device_app_commands_prepare(&explicit_allocator, &stream_config,
                                        &explicit_config) == H2_PAL_OK);
  assert(explicit_allocator.client_config.allocator == &mem);
  h2_loader_deinit(&explicit_allocator.serial_client.loader);
  h2_loader_image_identity_t identity_result;
  memset(&identity_result, 0xa5, sizeof(identity_result));
  assert(h2_device_app_current_identity(
             NULL, NULL, H2_LOADER_IMAGE_ROLE_APP, "board", "target", "1.2.3",
             &identity_result) == H2_PAL_ERR_INVALID_ARG);
  const h2_loader_image_identity_t empty_identity = {0};
  assert(!memcmp(&identity_result, &empty_identity, sizeof(identity_result)));
  assert(allocations == 0u);
  return 0;
}
