#include "app_config.h"

#include "os/os_api.h"

#include "hal/h2_pal_ble.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_device_service.h"
#include "h2_jieli_wl82_console.h"
#include "device/device.h"

#include <stdio.h>
#include "h2_atomic.h"
#include <string.h>

#include "system/task.h"
#include "system/timer.h"

static const uint8_t service_uuid_bytes[16] = {
    0x1d, 0x72, 0xa1, 0x6b, 0x3a, 0xaf, 0x0b, 0xaa,
    0xe2, 0x53, 0xd8, 0x3e, 0x70, 0xb5, 0xa4, 0x71,
};
static const uint8_t tx_uuid_bytes[16] = {
    0x1e, 0xcf, 0xd2, 0xbc, 0x8f, 0xd3, 0xb0, 0x98,
    0x70, 0x51, 0xfb, 0x56, 0x55, 0xa0, 0xd3, 0x46,
};
static const uint8_t rx_uuid_bytes[16] = {
    0xfe, 0x0e, 0xbc, 0xc9, 0xd6, 0x87, 0x36, 0xa5,
    0x8d, 0x5b, 0xf2, 0x05, 0x15, 0xad, 0x62, 0x8f,
};

static const h2_pal_ble_host_api_t* ble;
static uint16_t service_handle;
static uint16_t tx_value_handle;
static uint16_t tx_cccd_handle;
static uint16_t rx_value_handle;
static h2_atomic_uint_t write_count;
static int ble_result;
static h2_device_app_commands_t loader_commands;
static h2_runtime_t* command_runtime;
static int start_commands(void) {
  h2_runtime_config_t runtime_config;
  int rc;
  for (unsigned i = 0; !dev_online("sd0") && i < 500u; ++i) os_time_dly(1u);
  rc = h2_jieli_ac791n_devkit_runtime_config(&runtime_config);
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&runtime_config, &command_runtime);
  const h2_device_app_commands_config_t config = {
      .active_name = "pal-ble-smoke",
      .h2loader_partition_id = H2_JIELI_PARTITION_LOADER,
      .app_partition_id = H2_JIELI_PARTITION_APP,
      .coredump_partition_id = H2_JIELI_PARTITION_COREDUMP,
      .hardware_capabilities =
          H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
      .app_entry_path = "app/jieli/update.ufw",
      .boot_policy_flags =
          H2_DEVICE_BOOT_TRACK_TRIAL | H2_DEVICE_BOOT_PERSIST_RECOVERY |
          H2_DEVICE_BOOT_STORED_SHADOW | H2_DEVICE_BOOT_PREPARE_SELECTION,
      .identity_policy = H2_DEVICE_IDENTITY_COMMITTED,
      .serial_admission_fallback_ms = H2_DEVICE_APP_ADMISSION_FALLBACK_MS,
      .command_stack_size = 48u * 1024u,
      .ble_advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
  };
  if (rc == H2_PAL_OK)
    rc = h2_device_app_commands_prepare_runtime(&loader_commands,
                                                command_runtime, &config);
  if (rc == H2_PAL_OK)
    rc = h2_device_app_commands_start_serial(&loader_commands, NULL);
  return rc;
}

static void heartbeat_task(void* user) {
  (void)user;
  for (unsigned heartbeat = 1u;; ++heartbeat) {
    os_time_dly(100u);
    printf("H2_JIELI_PAL_BLE_HEARTBEAT count=%u writes=%u result=%d\r\n",
           heartbeat, h2_atomic_load_explicit(&write_count, H2_ATOMIC_RELAXED),
           ble_result);
  }
}

static void return_to_loader(void* user) {
  (void)user;
  /* Unconfirmed diagnostics return through Loader's trial reconciliation. */
  system_reset();
}

int h2_jieli_ac791n_devkit_early_app_boot(void) {
  uint16_t timer =
      sys_timeout_add_to_task("sys_timer", NULL, return_to_loader, 300000u);
  if (timer == 0u) {
    system_reset();
    return -1;
  }
  return 0;
}

static h2_pal_result_t on_write(void* user,
                                const h2_pal_ble_gatt_access_t* access,
                                const uint8_t* data, size_t len) {
  (void)user;
  unsigned count =
      h2_atomic_fetch_add_explicit(&write_count, 1u, H2_ATOMIC_RELAXED) + 1u;
  printf("H2_JIELI_PAL_BLE_WRITE count=%u conn=%u attr=%u bytes=%u\r\n", count,
         (unsigned)access->conn_handle, (unsigned)access->attr_handle,
         (unsigned)len);
  h2_pal_result_t result =
      h2_pal_ble_notify(ble, access->conn_handle, tx_value_handle, data, len);
  printf("H2_JIELI_PAL_BLE_ECHO result=%d\r\n", result);
  return H2_PAL_OK;
}

void app_main(void) {
  if (h2_atomic_init(&write_count, 0u) != H2_ATOMIC_OK) {
    return_to_loader(NULL);
    return;
  }
  int command_result = start_commands();
  printf("H2_JIELI_PAL_BLE_COMMANDS result=%d build=controlled-central-v1\r\n",
         command_result);
  if (command_result != H2_PAL_OK) {
    return_to_loader(NULL);
    return;
  }
  static h2_pal_ble_gatt_characteristic_t characteristics[2];
  static h2_pal_ble_gatt_service_t service;
  static const uint8_t diagnostic_marker[] = {
      'H', '2', 'P', 'A', 'L', '1', 0x01u,
  };
  const h2_pal_ble_uuid_t service_uuid = {.data = service_uuid_bytes,
                                          .len = sizeof(service_uuid_bytes)};
  const h2_pal_ble_adv_data_t adv_data = {
      .local_name = "H2PAL",
      .service_uuids = &service_uuid,
      .service_uuid_count = 1u,
      .manufacturer_data =
          {
              .data = diagnostic_marker,
              .len = sizeof(diagnostic_marker),
          },
  };
  const h2_pal_ble_adv_params_t adv_params = {
      .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
      .interval_min_ms = 100u,
      .interval_max_ms = 120u,
      .type = H2_PAL_BLE_ADV_TYPE_LEGACY,
      .primary_phy = H2_PAL_BLE_PHY_1M,
      .secondary_phy = H2_PAL_BLE_PHY_1M,
  };

  ble = h2_jieli_ac791n_devkit_ble_host_api(h2_jieli_wl82_platform_log_api());
  characteristics[0] = (h2_pal_ble_gatt_characteristic_t){
      .uuid = {.data = tx_uuid_bytes, .len = sizeof(tx_uuid_bytes)},
      .properties = H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
      .max_value_len = 196u,
      .out_value_handle = &tx_value_handle,
      .out_cccd_handle = &tx_cccd_handle,
  };
  characteristics[1] = (h2_pal_ble_gatt_characteristic_t){
      .uuid = {.data = rx_uuid_bytes, .len = sizeof(rx_uuid_bytes)},
      .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE |
                    H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP,
      .permissions = H2_PAL_BLE_GATT_PERMISSION_WRITE,
      .max_value_len = 196u,
      .write = on_write,
      .out_value_handle = &rx_value_handle,
  };
  service = (h2_pal_ble_gatt_service_t){
      .uuid = service_uuid,
      .primary = true,
      .characteristics = characteristics,
      .characteristic_count = 2u,
      .out_service_handle = &service_handle,
  };

  int result =
      h2_pal_system_event_init(h2_jieli_wl82_platform_system_event_api());
  printf("H2_JIELI_PAL_BLE step=system-event-init result=%d\r\n", result);
  if (result == H2_PAL_OK)
    result = h2_pal_ble_register_gatt_services(ble, &service, 1u);
  printf(
      "H2_JIELI_PAL_BLE step=gatt result=%d service=%u tx=%u cccd=%u rx=%u\r\n",
      result, (unsigned)service_handle, (unsigned)tx_value_handle,
      (unsigned)tx_cccd_handle, (unsigned)rx_value_handle);
  if (result == H2_PAL_OK) result = h2_pal_ble_start(ble);
  printf("H2_JIELI_PAL_BLE step=start result=%d\r\n", result);
  if (result == H2_PAL_OK) result = h2_pal_ble_set_adv_data(ble, &adv_data);
  printf("H2_JIELI_PAL_BLE step=adv-data result=%d\r\n", result);
  if (result == H2_PAL_OK) {
    result = h2_pal_ble_start_advertising(ble, &adv_params);
  }
  printf("H2_JIELI_PAL_BLE_READY result=%d name=H2PAL\r\n", result);
  ble_result = result;
  result = task_create(heartbeat_task, NULL, "h2pal/heartbeat");
  printf("H2_JIELI_PAL_BLE step=heartbeat-task result=%d\r\n", result);
}
