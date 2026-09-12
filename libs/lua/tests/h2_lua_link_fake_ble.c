#include "h2_lua_link_fake_ble.h"

#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local fake_terminal_gate_t *s_terminal_gate;

static h2_pal_result_t fake_sync_unlock(void *user, h2_pal_mutex_t *mutex) {
  const h2_pal_sync_api_t *sync = h2_desktop_platform_sync_api();
  h2_pal_result_t rc = sync->vtable->unlock_mutex(user, mutex);
  fake_terminal_gate_t *gate = s_terminal_gate;
  if (gate != NULL) {
    s_terminal_gate = NULL;
    pthread_mutex_lock(&gate->mutex);
    atomic_store(&gate->reached, 1);
    while (!gate->released) {
      pthread_cond_wait(&gate->cond, &gate->mutex);
    }
    pthread_mutex_unlock(&gate->mutex);
  }
  return rc;
}

/*
 * Two fake BLE Hosts joined by one "air". Device 0 and device 1 each have
 * their own system-event bus; connect, MTU exchange, GATT writes,
 * notifications and disconnects are delivered synchronously to the other
 * device's bus or GATT callbacks, like a radio that never loses a packet.
 */

static fake_device_t *fake_other(fake_device_t *device) {
  return &device->air->devices[1 - device->index];
}

static int fake_subscribe(void *user, h2_pal_system_event_type_t type,
                          h2_pal_system_event_handler_t handler,
                          void *handler_user,
                          h2_pal_system_event_subscription_t **out) {
  fake_device_t *device = user;
  pthread_mutex_lock(&device->bus_mutex);
  for (size_t i = 0u; i < FAKE_SUBSCRIPTION_MAX; ++i) {
    if (!device->subscriptions[i].used) {
      device->subscriptions[i] = (struct h2_pal_system_event_subscription){
          .type = type, .handler = handler, .user = handler_user, .used = 1};
      *out = &device->subscriptions[i];
      pthread_mutex_unlock(&device->bus_mutex);
      return H2_PAL_OK;
    }
  }
  pthread_mutex_unlock(&device->bus_mutex);
  return H2_PAL_ERR_FULL;
}

static void fake_unsubscribe(void *user,
                             h2_pal_system_event_subscription_t *subscription) {
  fake_device_t *device = user;
  if (subscription == NULL) {
    return;
  }
  pthread_mutex_lock(&device->bus_mutex);
  subscription->used = 0;
  pthread_mutex_unlock(&device->bus_mutex);
}

static const h2_pal_system_event_vtable_t s_fake_event_vtable = {
    .subscribe = fake_subscribe,
    .unsubscribe = fake_unsubscribe,
};

static void fake_post(fake_device_t *device, h2_pal_system_event_type_t type,
                      const void *payload, size_t payload_size) {
  struct h2_pal_system_event_subscription snapshot[FAKE_SUBSCRIPTION_MAX];
  size_t count = 0u;
  const h2_pal_system_event_t event = {
      .type = type, .payload = payload, .payload_size = payload_size};
  pthread_mutex_lock(&device->bus_mutex);
  for (size_t i = 0u; i < FAKE_SUBSCRIPTION_MAX; ++i) {
    if (device->subscriptions[i].used &&
        device->subscriptions[i].type == type) {
      snapshot[count++] = device->subscriptions[i];
    }
  }
  pthread_mutex_unlock(&device->bus_mutex);
  for (size_t i = 0u; i < count; ++i) {
    (void)snapshot[i].handler(snapshot[i].user, &event);
  }
}

/* Called with air->mutex held so a report never races stop_scan. The first
 * advertisement seen from the peer address in a scan is the only one
 * reported until the scan restarts. */
static void fake_report_locked(fake_device_t *scanner,
                               fake_device_t *advertiser) {
  static const uint8_t foreign_uuid[16] = {0x5au, 0x5au};
  const h2_pal_ble_uuid_t uuid =
      advertiser->adv_running
          ? (h2_pal_ble_uuid_t){advertiser->adv_uuid, advertiser->adv_uuid_len}
          : (h2_pal_ble_uuid_t){foreign_uuid, sizeof(foreign_uuid)};
  if (!scanner->scanning || scanner->scan_seen_peer ||
      (!advertiser->adv_running && !advertiser->foreign_adv)) {
    return;
  }
  scanner->scan_seen_peer = 1;
  h2_pal_ble_scan_result_t result = {
      .rssi = -40,
      .connectable = true,
      .service_uuids = &uuid,
      .service_uuid_count = 1u,
      .data_status = H2_PAL_BLE_ADV_DATA_COMPLETE,
      .tx_power = 127,
  };
  result.addr.value[0] = (uint8_t)advertiser->index;
  result.addr.type = H2_PAL_BLE_ADDR_TYPE_RANDOM;
  if (scanner->scan_cb(scanner->scan_user, &result)) {
    scanner->scanning = 0;
  }
}

static h2_pal_result_t fake_adv_set_create(void *user,
                                           const h2_pal_ble_adv_params_t *params,
                                           h2_pal_ble_adv_set_t **out_set) {
  fake_device_t *device = user;
  assert(params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE);
  *out_set = malloc(sizeof(**out_set));
  assert(*out_set != NULL);
  (*out_set)->device = device;
  pthread_mutex_lock(&device->air->mutex);
  device->adv_sets++;
  pthread_mutex_unlock(&device->air->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_set_data(void *user,
                                             h2_pal_ble_adv_set_t *set,
                                             const h2_pal_ble_adv_data_t *data) {
  fake_device_t *device = user;
  assert(set->device == device && data->service_uuid_count == 1u);
  pthread_mutex_lock(&device->air->mutex);
  device->adv_uuid_len = data->service_uuids[0].len;
  memcpy(device->adv_uuid, data->service_uuids[0].data, device->adv_uuid_len);
  pthread_mutex_unlock(&device->air->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_start(void *user,
                                          h2_pal_ble_adv_set_t *set) {
  fake_device_t *device = user;
  (void)set;
  pthread_mutex_lock(&device->air->mutex);
  device->adv_running = 1;
  fake_report_locked(fake_other(device), device);
  pthread_mutex_unlock(&device->air->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_stop(void *user,
                                         h2_pal_ble_adv_set_t *set) {
  fake_device_t *device = user;
  (void)set;
  pthread_mutex_lock(&device->air->mutex);
  device->adv_running = 0;
  pthread_mutex_unlock(&device->air->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_destroy(void *user,
                                            h2_pal_ble_adv_set_t *set) {
  fake_device_t *device = user;
  pthread_mutex_lock(&device->air->mutex);
  device->adv_running = 0;
  device->adv_sets--;
  pthread_mutex_unlock(&device->air->mutex);
  free(set);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_start_scan(void *user,
                                       const h2_pal_ble_scan_params_t *params,
                                       h2_pal_ble_scan_result_fn on_result,
                                       void *scan_user) {
  fake_device_t *device = user;
  (void)params;
  pthread_mutex_lock(&device->air->mutex);
  device->scanning = 1;
  device->scan_seen_peer = 0;
  device->scan_starts++;
  device->scan_cb = on_result;
  device->scan_user = scan_user;
  fake_report_locked(device, fake_other(device));
  pthread_mutex_unlock(&device->air->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_stop_scan(void *user) {
  fake_device_t *device = user;
  pthread_mutex_lock(&device->air->mutex);
  device->scanning = 0;
  pthread_mutex_unlock(&device->air->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_register(void *user,
                                     const h2_pal_ble_gatt_service_t *services,
                                     size_t count) {
  fake_device_t *device = user;
  size_t slot;
  /* KCP TX, KCP RX and the datagram characteristic in one service. */
  assert(count == 1u && services[0].characteristic_count == 3u);
  assert(services[0].uuid.len == 16u);
  /* A known UUID reattaches callbacks to its retained slot; a new one takes
   * a slot for good, as the GATT table only grows. */
  pthread_mutex_lock(&device->air->mutex);
  for (slot = 0u; slot < device->retained_count; ++slot) {
    if (memcmp(device->retained_uuid[slot], services[0].uuid.data, 16u) ==
        0) {
      break;
    }
  }
  if (slot == device->retained_count) {
    if (slot == FAKE_GATT_SERVICE_SLOTS) {
      pthread_mutex_unlock(&device->air->mutex);
      return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(device->retained_uuid[slot], services[0].uuid.data, 16u);
    device->retained_count++;
  }
  device->registrations++;
  pthread_mutex_unlock(&device->air->mutex);
  if (services[0].out_service_handle != NULL) {
    *services[0].out_service_handle = 1u;
  }
  for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
    const h2_pal_ble_gatt_characteristic_t *ch =
        &services[0].characteristics[i];
    if (ch->out_value_handle != NULL) {
      *ch->out_value_handle = (uint16_t)(2u + 2u * i);
    }
    if (ch->out_cccd_handle != NULL) {
      *ch->out_cccd_handle = (uint16_t)(3u + 2u * i);
    }
  }
  pthread_mutex_lock(&device->air->mutex);
  device->service = services;
  pthread_mutex_unlock(&device->air->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_unregister(void *user) {
  fake_device_t *device = user;
  pthread_mutex_lock(&device->air->mutex);
  device->service = NULL;
  pthread_mutex_unlock(&device->air->mutex);
  s_terminal_gate = device->terminal_gate;
  return H2_PAL_OK;
}

static uint16_t fake_handle(const fake_device_t *device) {
  return device->conn_handle;
}

static h2_pal_result_t fake_connect(void *user, const h2_pal_ble_addr_t *addr,
                                    const h2_pal_ble_connect_params_t *params,
                                    uint16_t *out_conn_handle) {
  fake_device_t *central = user;
  fake_device_t *peripheral = fake_other(central);
  assert(params->supervision_timeout_ms != 0u);
  pthread_mutex_lock(&central->air->mutex);
  if (addr->value[0] != (uint8_t)peripheral->index ||
      !peripheral->adv_running || central->connected) {
    pthread_mutex_unlock(&central->air->mutex);
    return H2_PAL_ERR_TIMEOUT;
  }
  /* A connectable advertising set stops when a central connects. */
  peripheral->adv_running = 0;
  peripheral->connected = 1;
  central->connected = 1;
  peripheral->conn_handle = FAKE_PERIPHERAL_HANDLE;
  central->conn_handle = FAKE_CENTRAL_HANDLE;
  central->connects++;
  pthread_mutex_unlock(&central->air->mutex);
  const h2_pal_ble_connection_t to_peripheral = {
      .conn_handle = FAKE_PERIPHERAL_HANDLE,
      .role = H2_PAL_BLE_ROLE_PERIPHERAL,
      .mtu = 23u,
  };
  const h2_pal_ble_connection_t to_central = {
      .conn_handle = FAKE_CENTRAL_HANDLE,
      .role = H2_PAL_BLE_ROLE_CENTRAL,
      .mtu = 23u,
  };
  fake_post(peripheral, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &to_peripheral,
            sizeof(to_peripheral));
  fake_post(central, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &to_central,
            sizeof(to_central));
  *out_conn_handle = FAKE_CENTRAL_HANDLE;
  return H2_PAL_OK;
}

void fake_drop_link(fake_air_t *air) {
  pthread_mutex_lock(&air->mutex);
  const int was_connected = air->devices[0].connected;
  air->devices[0].connected = 0;
  air->devices[1].connected = 0;
  pthread_mutex_unlock(&air->mutex);
  if (!was_connected) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    const h2_pal_ble_disconnected_info_t info = {
        .conn_handle = fake_handle(&air->devices[i]),
        .reason = 0x08,
    };
    fake_post(&air->devices[i], H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
              &info, sizeof(info));
  }
}

static h2_pal_result_t fake_disconnect(void *user, uint16_t conn_handle) {
  fake_device_t *device = user;
  if (conn_handle != fake_handle(device)) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  fake_drop_link(device->air);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_exchange_mtu(void *user, uint16_t conn_handle,
                                         uint16_t *out_mtu,
                                         uint32_t timeout_ms) {
  fake_device_t *central = user;
  const h2_pal_ble_mtu_info_t info = {.conn_handle = FAKE_PERIPHERAL_HANDLE,
                                      .mtu = FAKE_MTU};
  (void)timeout_ms;
  if (conn_handle != FAKE_CENTRAL_HANDLE) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  fake_post(fake_other(central), H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
            &info, sizeof(info));
  *out_mtu = FAKE_MTU;
  return H2_PAL_OK;
}

static h2_pal_result_t
fake_discover(void *user, uint16_t conn_handle,
              const h2_pal_ble_gatt_discovery_request_t *request,
              h2_pal_ble_gatt_discovery_entry_t *entries, size_t max_entries,
              size_t *out_count, uint32_t timeout_ms) {
  fake_device_t *central = user;
  const h2_pal_ble_gatt_service_t *service;
  static const uint8_t cccd_uuid[] = {0x02u, 0x29u};
  (void)conn_handle;
  (void)timeout_ms;
  *out_count = 0u;
  pthread_mutex_lock(&central->air->mutex);
  service = fake_other(central)->service;
  pthread_mutex_unlock(&central->air->mutex);
  if (service == NULL || max_entries == 0u) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_SERVICE) {
    if (request->uuid_filter.len != service->uuid.len ||
        memcmp(request->uuid_filter.data, service->uuid.data,
               service->uuid.len) != 0) {
      return H2_PAL_ERR_NOT_FOUND;
    }
    entries[0] = (h2_pal_ble_gatt_discovery_entry_t){
        .kind = request->kind,
        .uuid = service->uuid,
        .start_handle = 1u,
        .end_handle = 7u,
    };
    *out_count = 1u;
    return H2_PAL_OK;
  }
  if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC) {
    for (size_t i = 0u; i < service->characteristic_count; ++i) {
      const h2_pal_ble_gatt_characteristic_t *ch = &service->characteristics[i];
      if (ch->uuid.len == request->uuid_filter.len &&
          memcmp(ch->uuid.data, request->uuid_filter.data, ch->uuid.len) ==
              0) {
        entries[0] = (h2_pal_ble_gatt_discovery_entry_t){
            .kind = request->kind,
            .uuid = ch->uuid,
            .value_handle = (uint16_t)(2u + 2u * i),
            .properties = ch->properties,
        };
        *out_count = 1u;
        return H2_PAL_OK;
      }
    }
    return H2_PAL_ERR_NOT_FOUND;
  }
  /* Each characteristic's CCCD directly follows its value handle. */
  entries[0] = (h2_pal_ble_gatt_discovery_entry_t){
      .kind = request->kind,
      .uuid = {cccd_uuid, sizeof(cccd_uuid)},
      .value_handle = request->start_handle,
  };
  *out_count = 1u;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_subscribe_gatt(
    void *user, uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe, uint32_t timeout_ms) {
  fake_device_t *central = user;
  const h2_pal_ble_subscription_state_t state = {
      .conn_handle = FAKE_PERIPHERAL_HANDLE,
      .value_handle = subscribe->value_handle,
      .mode = subscribe->mode,
      .enabled = subscribe->enable,
  };
  int connected;
  (void)conn_handle;
  (void)timeout_ms;
  pthread_mutex_lock(&central->air->mutex);
  connected = central->connected;
  pthread_mutex_unlock(&central->air->mutex);
  if (!connected) {
    return H2_PAL_ERR_CLOSED;
  }
  fake_post(fake_other(central),
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED, &state,
            sizeof(state));
  return H2_PAL_OK;
}

static h2_pal_result_t fake_gatt_write(void *user, uint16_t conn_handle,
                                       uint16_t attr_handle,
                                       const uint8_t *data, size_t len,
                                       bool with_response,
                                       uint32_t timeout_ms) {
  fake_device_t *central = user;
  const h2_pal_ble_gatt_service_t *service;
  (void)conn_handle;
  (void)with_response;
  (void)timeout_ms;
  pthread_mutex_lock(&central->air->mutex);
  service = central->connected ? fake_other(central)->service : NULL;
  pthread_mutex_unlock(&central->air->mutex);
  if (service == NULL) {
    return H2_PAL_ERR_CLOSED;
  }
  for (size_t i = 0u; i < service->characteristic_count; ++i) {
    const h2_pal_ble_gatt_characteristic_t *ch = &service->characteristics[i];
    if ((uint16_t)(2u + 2u * i) == attr_handle && ch->write != NULL) {
      const h2_pal_ble_gatt_access_t access = {
          .conn_handle = FAKE_PERIPHERAL_HANDLE,
          .attr_handle = attr_handle,
      };
      h2_pal_result_t rc = ch->write(ch->user, &access, data, len);
      /* A server without a session yet drops the write; KCP retransmits. */
      return rc == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : rc;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t fake_notify(void *user, uint16_t conn_handle,
                                   uint16_t attr_handle, const uint8_t *data,
                                   size_t len) {
  fake_device_t *peripheral = user;
  h2_pal_ble_gatt_client_value_t value = {
      .conn_handle = FAKE_CENTRAL_HANDLE,
      .attr_handle = attr_handle,
      .value_len = len,
  };
  int connected;
  (void)conn_handle;
  pthread_mutex_lock(&peripheral->air->mutex);
  connected = peripheral->connected;
  pthread_mutex_unlock(&peripheral->air->mutex);
  if (!connected) {
    return H2_PAL_ERR_CLOSED;
  }
  assert(len <= sizeof(value.value));
  memcpy(value.value, data, len);
  fake_post(fake_other(peripheral),
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION, &value,
            sizeof(value));
  return H2_PAL_OK;
}

static const h2_pal_ble_vtable_t s_fake_ble_vtable = {
    .adv_set_create = fake_adv_set_create,
    .adv_set_set_data = fake_adv_set_set_data,
    .adv_set_start = fake_adv_set_start,
    .adv_set_stop = fake_adv_set_stop,
    .adv_set_destroy = fake_adv_set_destroy,
    .start_scan = fake_start_scan,
    .stop_scan = fake_stop_scan,
    .register_gatt_services = fake_register,
    .unregister_gatt_services = fake_unregister,
    .notify = fake_notify,
    .connect = fake_connect,
    .disconnect = fake_disconnect,
    .exchange_mtu = fake_exchange_mtu,
    .gatt_discover = fake_discover,
    .gatt_write = fake_gatt_write,
    .gatt_subscribe = fake_subscribe_gatt,
};

void fake_air_init(fake_air_t *air) {
  memset(air, 0, sizeof(*air));
  pthread_mutex_init(&air->mutex, NULL);
  for (int i = 0; i < 2; ++i) {
    fake_device_t *device = &air->devices[i];
    device->air = air;
    device->index = i;
    device->sync = *h2_desktop_platform_sync_api();
    device->sync_vtable = *device->sync.vtable;
    device->sync_vtable.unlock_mutex = fake_sync_unlock;
    device->sync.vtable = &device->sync_vtable;
    /* Another service (e.g. a management service) already holds one slot
     * and keeps advertising from the same address. */
    memset(device->retained_uuid[0], 0xa5, 16u);
    device->retained_count = 1u;
    device->foreign_adv = 1;
    pthread_mutex_init(&device->bus_mutex, NULL);
    device->ble = (h2_pal_ble_host_api_t){
        .user = device,
        .vtable = &s_fake_ble_vtable,
        .allocator = h2_desktop_platform_default_allocator(),
    };
    device->events = (h2_pal_system_event_api_t){
        .user = device,
        .vtable = &s_fake_event_vtable,
    };
  }
}


fake_snapshot_t fake_snapshot(fake_device_t *device) {
  fake_snapshot_t value;
  pthread_mutex_lock(&device->air->mutex);
  value.adv_sets = device->adv_sets;
  value.adv_running = device->adv_running;
  value.scanning = device->scanning;
  value.registered = device->service != NULL;
  value.connected = device->connected;
  pthread_mutex_unlock(&device->air->mutex);
  value.subscriptions = 0;
  pthread_mutex_lock(&device->bus_mutex);
  for (size_t i = 0u; i < FAKE_SUBSCRIPTION_MAX; ++i) {
    value.subscriptions += device->subscriptions[i].used;
  }
  pthread_mutex_unlock(&device->bus_mutex);
  return value;
}

int fake_is_released(fake_device_t *device) {
  const fake_snapshot_t value = fake_snapshot(device);
  return value.adv_sets == 0 && !value.adv_running && !value.scanning &&
         !value.registered && !value.connected &&
         value.subscriptions == device->baseline_subscriptions;
}

/* ---- Runtime and Lua Host assembly ---- */

void fake_set_baseline(fake_device_t *device) {
  device->baseline_subscriptions = fake_snapshot(device).subscriptions;
}

h2_runtime_t *fake_create_runtime(const h2_pal_ble_host_api_t *ble,
                                    const h2_pal_system_event_api_t *events) {
  fake_device_t *device = ble->user;
  h2_runtime_config_t config = {
      .board = "test",
      .target = "desktop",
      .chip = "host",
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .mem = h2_desktop_platform_default_allocator(),
      .log = h2_desktop_platform_log_api(),
      .time = h2_desktop_platform_time_api(),
      .timer = h2_pal_unsupported_timer_api(),
      .task = h2_desktop_platform_task_api(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = ble->vtable == &s_fake_ble_vtable
                  ? &device->sync
                  : h2_desktop_platform_sync_api(),
      .fs = h2_pal_unsupported_fs_api(),
      .disk = h2_pal_unsupported_disk_api(),
      .pref = h2_pal_unsupported_pref_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .http = h2_pal_unsupported_http_api(),
      /* No Wi-Fi or network: the link must work over BLE alone. */
      .net = h2_pal_unsupported_net_api(),
      .netif = h2_pal_unsupported_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
      .ble_host = ble,
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_pal_unsupported_power_api(),
      .display = h2_pal_unsupported_display_api(),
      .audio = h2_pal_unsupported_audio_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_pal_unsupported_periph_api(),
      .button = h2_pal_unsupported_button_api(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .input = h2_pal_unsupported_input_api(),
      .system_event = events,
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  return runtime;
}

