#include "h2loader_bleikcp_internal.h"
#include "h2loader_app_task_names.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

const char h2loader_ble_command_task_name[] =
    H2LOADER_BLE_COMMAND_TASK_NAME_VALUE;

struct h2_loader_ble_service {
  h2_loader_ble_service_config_t config;
};

static struct h2_loader_ble_service services[2];
static unsigned service_count;
static unsigned live_allocations;
static unsigned global_refs;
static unsigned task_starts;
static unsigned task_joins;
static unsigned join_failures;
static unsigned service_closes;
static unsigned close_failures;
static unsigned shutdown_failures;
static unsigned sleeps;
static int open_result;
static bool partial_open;
static int command_init_result;
static int command_results[8];
static unsigned command_result_count;
static unsigned command_polls;
static size_t command_stack_size;
static int task_marker;
static int stream_marker;

static void* allocate(void* user, size_t size) {
  (void)user;
  void* result = malloc(size);
  if (result != NULL) ++live_allocations;
  return result;
}

static void release(void* user, void* pointer) {
  (void)user;
  assert(pointer != NULL && live_allocations != 0u);
  --live_allocations;
  free(pointer);
}

static int start_task(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* context,
                      h2_pal_task_t** out_task) {
  (void)user;
  assert(strcmp(options->name, H2LOADER_BLE_COMMAND_TASK_NAME_VALUE) == 0);
  ++task_starts;
  command_stack_size = options->min_stack_size;
  entry(context);
  *out_task = (h2_pal_task_t*)&task_marker;
  return H2_PAL_OK;
}

static int join_task(void* user, h2_pal_task_t* task) {
  (void)user;
  assert(task == (h2_pal_task_t*)&task_marker);
  ++task_joins;
  if (join_failures != 0u) {
    --join_failures;
    return H2_PAL_ERR_TASK;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t sleep_ms(void* user, uint32_t delay) {
  (void)user;
  assert(delay == 10u);
  ++sleeps;
  return H2_PAL_OK;
}

int h2_bleikcp_global_init(void) {
  ++global_refs;
  return H2_PAL_OK;
}

int h2_bleikcp_global_shutdown(void) {
  assert(global_refs != 0u);
  if (shutdown_failures != 0u) {
    --shutdown_failures;
    return H2_PAL_ERR_BUSY;
  }
  --global_refs;
  return H2_PAL_OK;
}

int h2_loader_ble_service_open(const h2_loader_ble_service_config_t* config,
                               h2_loader_ble_service_t** out_service) {
  assert(service_count < 2u);
  *out_service = NULL;
  if (open_result == H2_PAL_OK || partial_open) {
    services[service_count].config = *config;
    *out_service = &services[service_count++];
  }
  return open_result;
}

int h2_loader_ble_service_close(h2_loader_ble_service_t* service) {
  assert(service >= services && service < services + service_count);
  ++service_closes;
  if (close_failures != 0u) {
    --close_failures;
    return H2_PAL_ERR_TASK;
  }
  return H2_PAL_OK;
}

h2_command_io_api_t h2_loader_ble_command_io(h2_bleikcp_t* stream) {
  assert(stream == (h2_bleikcp_t*)&stream_marker);
  return (h2_command_io_api_t){.user = stream};
}

int h2_loader_command_init(h2_loader_command_t* command,
                           const h2_loader_command_config_t* config) {
  assert(config->transport == H2_LOADER_COMMAND_TRANSPORT_BLE);
  assert(config->io.user == &stream_marker);
  command->config = *config;
  return command_init_result;
}

int h2_loader_command_poll(h2_loader_command_t* command, uint32_t timeout_ms) {
  assert(command->config.transport == H2_LOADER_COMMAND_TRANSPORT_BLE);
  assert(timeout_ms == 50u && command_polls < command_result_count);
  return command_results[command_polls++];
}

static const h2_pal_mem_vtable_t memory_vtable = {
    .alloc = allocate, .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};
static const h2_pal_task_vtable_t task_vtable = {
    .start = start_task, .join = join_task};
static const h2_pal_task_api_t task = {.vtable = &task_vtable};
static const h2_pal_time_vtable_t time_vtable = {.sleep_ms = sleep_ms};
static const h2_pal_time_api_t time_api = {.vtable = &time_vtable};
static h2_runtime_t runtime = {
    .mem = &memory, .task = &task, .time = &time_api};

static void reset(void) {
  assert(live_allocations == 0u && global_refs == 0u);
  memset(services, 0, sizeof(services));
  service_count = task_starts = task_joins = service_closes = sleeps = 0u;
  join_failures = close_failures = shutdown_failures = 0u;
  open_result = command_init_result = H2_PAL_OK;
  partial_open = false;
  command_polls = 0u;
  command_results[0] = H2_PAL_ERR_TIMEOUT;
  command_results[1] = H2_PAL_ERR_WOULD_BLOCK;
  command_results[2] = H2_PAL_OK;
  command_results[3] = H2_PAL_ERR_CLOSED;
  command_result_count = 4u;
}

static void test_session(const h2loader_ble_command_service_config_t* config,
                         bool use_default_factory) {
  reset();
  h2loader_app_command_service_api_t configured;
  assert(h2loader_bleikcp_command_service_with_config(config, &configured) ==
         H2_PAL_OK);
  const h2loader_app_command_service_api_t* api =
      use_default_factory ? h2loader_bleikcp_command_service() : &configured;
  h2_loader_command_config_t command = {0};
  void* service = NULL;
  assert(api->open(api->user, &runtime, &command, "test", 0u, &service) ==
         H2_PAL_OK);
  assert(service != NULL && global_refs == 1u && live_allocations == 1u);
  assert(services[0].config.server_task_stack_size ==
         (config != NULL ? config->server_task_stack_size : 0u));
  assert(services[0].config.stream_buffer_size ==
         (config != NULL ? config->stream_buffer_size : 0u));
  assert(services[0].config.stream_window ==
         (config != NULL ? config->stream_window : 0u));
  assert(services[0].config.input_frame_capacity ==
         (config != NULL ? config->input_frame_capacity : 0u));
  join_failures = 1u;
  assert(services[0].config.handler(services[0].config.handler_user,
                                   (h2_bleikcp_t*)&stream_marker, 7u) ==
         H2_PAL_OK);
  assert(command_polls == 4u);
  if (config != NULL && config->inline_commands) {
    assert(task_starts == 0u && task_joins == 0u && sleeps == 0u);
    command_polls = 0u;
    command_results[0] = H2_PAL_ERR_IO;
    assert(services[0].config.handler(services[0].config.handler_user,
                                     (h2_bleikcp_t*)&stream_marker, 7u) ==
           H2_PAL_ERR_IO);
    command_init_result = H2_PAL_ERR_NO_MEMORY;
    assert(services[0].config.handler(services[0].config.handler_user,
                                     (h2_bleikcp_t*)&stream_marker, 7u) ==
           H2_PAL_ERR_NO_MEMORY);
  } else {
    assert(task_starts == 1u && task_joins == 2u && sleeps == 1u);
    assert(command_stack_size == 49152u);
  }
  close_failures = 1u;
  assert(api->close(api->user, service) == H2_PAL_ERR_TASK);
  assert(global_refs == 1u && live_allocations == 1u);
  shutdown_failures = 1u;
  assert(api->close(api->user, service) == H2_PAL_ERR_BUSY);
  assert(service_closes == 2u && global_refs == 1u);
  assert(api->close(api->user, service) == H2_PAL_OK);
  assert(service_closes == 2u && global_refs == 0u && live_allocations == 0u);
}

static void test_open_cleanup(void) {
  reset();
  const h2loader_app_command_service_api_t* api =
      h2loader_bleikcp_command_service();
  h2_loader_command_config_t command = {0};
  void* service = NULL;
  open_result = H2_PAL_ERR_NO_MEMORY;
  assert(api->open(api->user, &runtime, &command, "test", 0u, &service) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(service == NULL && live_allocations == 0u && global_refs == 0u);
  partial_open = true;
  close_failures = 2u;
  assert(api->open(api->user, &runtime, &command, "test", 0u, &service) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(service != NULL && live_allocations == 1u && global_refs == 1u);
  assert(api->close(api->user, service) == H2_PAL_ERR_TASK);
  assert(api->close(api->user, service) == H2_PAL_OK);
  assert(live_allocations == 0u && global_refs == 0u);
}

int main(void) {
  h2loader_app_command_service_api_t api;
  h2loader_ble_command_service_config_t invalid = {
      .inline_commands = true, .server_task_stack_size = 32767u};
  memset(&api, 0xff, sizeof(api));
  assert(h2loader_bleikcp_command_service_with_config(&invalid, &api) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(api.open == NULL && api.close == NULL && api.user == NULL);
  const h2loader_ble_command_service_config_t defaults = {0};
  const h2loader_ble_command_service_config_t inline_config = {
      .inline_commands = true, .server_task_stack_size = 32768u};
  test_session(NULL, true);
  test_session(&defaults, false);
  test_session(&inline_config, false);
  const h2loader_ble_command_service_config_t compact = {
      .inline_commands = true,
      .server_task_stack_size = 32768u,
      .stream_buffer_size = 2048u,
      .stream_window = 8u,
      .input_frame_capacity = 8u};
  test_session(&compact, false);
  test_open_cleanup();
  return 0;
}
