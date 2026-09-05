#include "jieli_h2loader_app_support.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_loader_ble.h"
#include "h2loader_app_task_names.h"

/* Shared command state (disk, digest and operation mutex) is borrowed from
 * the UART configuration. Each BLE connection owns only its console client. */
static h2_loader_app_client_config_t ble_client_config;
static h2_loader_ble_service_t *ble_service;

static int handle_session(void *user, h2_bleikcp_t *stream,
                          uint16_t conn_handle) {
  (void)conn_handle;
  h2_loader_app_client_t client;
  int rc = h2_loader_app_client_init(&client, user);
  if (rc != H2_PAL_OK) return rc;
  const h2_loader_app_client_return_console_config_t console = {
      .client = &client,
      .task = h2_jieli_wl82_platform_task_api(),
      .read_user = stream,
      .read_byte = h2_loader_ble_app_read_byte,
      .write_user = stream,
      .write = h2_loader_ble_app_write,
      .task_name = H2LOADER_BLE_COMMAND_TASK_NAME_VALUE,
      .stack_size = 49152u,
  };
  rc = h2_loader_app_client_start_return_console(&console);
  if (rc != H2_PAL_OK) return rc;
  return h2_loader_app_client_join_return_console(&client);
}

int h2_jieli_app_loader_ble_start(
    const h2_loader_app_client_config_t *config) {
  if (config == NULL || config->operation_sync == NULL ||
      config->operation_mutex == NULL ||
      (config->hardware_capabilities & H2_LOADER_CAPABILITY_BLE) == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (ble_service != NULL) return H2_PAL_ERR_INVALID_STATE;
  ble_client_config = *config;
  const h2_loader_ble_service_config_t service = {
      .api = {
          .ble = h2_jieli_ac791n_devkit_ble_host_api(),
          .task = h2_jieli_wl82_platform_task_api(),
          .time = h2_jieli_wl82_platform_time_api(),
          .sync = h2_jieli_wl82_platform_sync_api(),
          .system_event = h2_jieli_wl82_platform_system_event_api(),
          .allocator = h2_jieli_wl82_platform_mem_api(),
      },
      .board = config->board,
      .capabilities = config->hardware_capabilities,
      .advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
      .handler = handle_session,
      .handler_user = &ble_client_config,
  };
  return h2_loader_ble_service_open(&service, &ble_service);
}
