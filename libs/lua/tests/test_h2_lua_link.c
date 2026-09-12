#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"
#include "h2_lua.h"
#include "h2_lua_capability.h"
#include "h2_lua_job.h"
#include "h2_lua_link.h"
#include "h2_pal.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Two fake BLE Hosts joined by one "air". Device 0 and device 1 each have
 * their own system-event bus; connect, MTU exchange, GATT writes,
 * notifications and disconnects are delivered synchronously to the other
 * device's bus or GATT callbacks, like a radio that never loses a packet.
 */

#define FAKE_SUBSCRIPTION_MAX 128u
#define FAKE_PERIPHERAL_HANDLE 7u
#define FAKE_CENTRAL_HANDLE 9u
#define FAKE_MTU 247u

struct h2_pal_system_event_subscription {
  h2_pal_system_event_type_t type;
  h2_pal_system_event_handler_t handler;
  void *user;
  int used;
};

typedef struct fake_air fake_air_t;

typedef struct fake_device {
  fake_air_t *air;
  int index;
  h2_pal_ble_host_api_t ble;
  h2_pal_system_event_api_t events;
  pthread_mutex_t bus_mutex;
  struct h2_pal_system_event_subscription subscriptions[FAKE_SUBSCRIPTION_MAX];
  /* Guarded by air->mutex. */
  int adv_sets;
  int adv_running;
  uint8_t adv_uuid[16];
  size_t adv_uuid_len;
  int scanning;
  h2_pal_ble_scan_result_fn scan_cb;
  void *scan_user;
  const h2_pal_ble_gatt_service_t *service;
  int connected;
  int connects;
  /* Subscriptions the Runtime itself keeps; the link must return to it. */
  int baseline_subscriptions;
} fake_device_t;

struct h2_pal_ble_adv_set {
  fake_device_t *device;
};

struct fake_air {
  pthread_mutex_t mutex;
  fake_device_t devices[2];
};

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

/* Called with air->mutex held so a report never races stop_scan. */
static void fake_report_locked(fake_device_t *scanner,
                               fake_device_t *advertiser) {
  const h2_pal_ble_uuid_t uuid = {advertiser->adv_uuid,
                                  advertiser->adv_uuid_len};
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
  if (scanner->scanning && advertiser->adv_running &&
      scanner->scan_cb(scanner->scan_user, &result)) {
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
  assert(count == 1u && services[0].characteristic_count == 2u);
  if (services[0].out_service_handle != NULL) {
    *services[0].out_service_handle = 1u;
  }
  for (size_t i = 0u; i < 2u; ++i) {
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
  return H2_PAL_OK;
}

static uint16_t fake_handle(const fake_device_t *device) {
  return device->index == 0 ? FAKE_PERIPHERAL_HANDLE : FAKE_CENTRAL_HANDLE;
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

static void fake_drop_link(fake_air_t *air) {
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
        .end_handle = 5u,
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
  entries[0] = (h2_pal_ble_gatt_discovery_entry_t){
      .kind = request->kind,
      .uuid = {cccd_uuid, sizeof(cccd_uuid)},
      .value_handle = 3u,
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

static void fake_air_init(fake_air_t *air) {
  memset(air, 0, sizeof(*air));
  pthread_mutex_init(&air->mutex, NULL);
  for (int i = 0; i < 2; ++i) {
    fake_device_t *device = &air->devices[i];
    device->air = air;
    device->index = i;
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

typedef struct fake_snapshot {
  int adv_sets;
  int adv_running;
  int scanning;
  int registered;
  int connected;
  int subscriptions;
} fake_snapshot_t;

static fake_snapshot_t fake_snapshot(fake_device_t *device) {
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

static int fake_is_released(fake_device_t *device) {
  const fake_snapshot_t value = fake_snapshot(device);
  return value.adv_sets == 0 && !value.adv_running && !value.scanning &&
         !value.registered && !value.connected &&
         value.subscriptions == device->baseline_subscriptions;
}

/* ---- Runtime and Lua Host assembly ---- */

static void fake_set_baseline(fake_device_t *device) {
  device->baseline_subscriptions = fake_snapshot(device).subscriptions;
}

static h2_runtime_t *create_runtime(const h2_pal_ble_host_api_t *ble,
                                    const h2_pal_system_event_api_t *events) {
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
      .sync = h2_desktop_platform_sync_api(),
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

static atomic_int s_marks[2];

/* capability.call('mark', ...) lets a script tell the test it reached a
 * checkpoint without the test reading Lua state. */
static h2_pal_result_t mark_call(void *user, h2_lua_capability_request_id_t id,
                                 const char *input, const char *options,
                                 char *output, size_t output_capacity,
                                 const char **out_error) {
  (void)id;
  (void)input;
  (void)options;
  (void)out_error;
  atomic_fetch_add((atomic_int *)user, 1);
  (void)snprintf(output, output_capacity, "ok");
  return H2_PAL_OK;
}

static h2_lua_host_t *create_host(h2_runtime_t *runtime, int enable_link,
                                  atomic_int *mark) {
  const h2_lua_host_config_t config = {
      .runtime = runtime,
      .worker_count = 2u,
      .max_jobs = 2u,
      .execution_timeout_ms = 60000u,
  };
  const h2_lua_link_config_t link_config = {
      .adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY,
      .scan_type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
  };
  h2_lua_host_t *host = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  if (enable_link) {
    assert(h2_lua_link_enable(host, &link_config) == H2_PAL_OK);
    assert(h2_lua_link_enable(host, &link_config) == H2_PAL_ERR_INVALID_STATE);
  }
  if (mark != NULL) {
    assert(h2_lua_register_capability(host, "mark", mark_call, NULL, mark) ==
           H2_PAL_OK);
  }
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  return host;
}

static h2_lua_job_id_t submit(h2_lua_host_t *host, const char *name,
                              const char *script, const char *tag) {
  const h2_lua_arg_t args[] = {{"tag", tag}};
  h2_lua_job_id_t job = H2_LUA_JOB_ID_NONE;
  assert(h2_lua_job_submit_text(host, name, (const uint8_t *)script,
                                strlen(script), args, 1u,
                                &job) == H2_PAL_OK);
  return job;
}

static int is_terminal(h2_lua_job_state_t state) {
  return state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
         state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
         state == H2_LUA_JOB_STOPPED;
}

static h2_lua_job_status_t wait_job(h2_lua_host_t *host, h2_lua_job_id_t job) {
  h2_lua_job_status_t status;
  for (int i = 0; i < 20000; ++i) {
    assert(h2_lua_job_get_status(host, job, &status) == H2_PAL_OK);
    if (is_terminal(status.state)) {
      return status;
    }
    (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
  }
  assert(!"Lua job did not finish");
  return status;
}

static void expect_success(h2_lua_host_t *host, h2_lua_job_id_t job,
                           const char *message) {
  const h2_lua_job_status_t status = wait_job(host, job);
  if (status.state != H2_LUA_JOB_SUCCEEDED ||
      strcmp(status.message, message) != 0) {
    fprintf(stderr, "job %u: state=%d message=%s (expected %s)\n",
            (unsigned)job, (int)status.state, status.message, message);
    abort();
  }
  assert(h2_lua_job_release(host, job) == H2_PAL_OK);
}

static void wait_until(int (*predicate)(void *), void *user) {
  for (int i = 0; i < 20000; ++i) {
    if (predicate(user)) {
      return;
    }
    (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
  }
  assert(!"condition not reached");
}

static int mark_reached(void *user) {
  return atomic_load((atomic_int *)user) > 0;
}

static int device_released(void *user) {
  return fake_is_released(user);
}

/* ---- Lua scripts ---- */

#define LUA_PRELUDE                                                           \
  "local link=require('link');local rt=require('runtime');"                   \
  "local ev=rt.event;local s={msgs={}};"                                      \
  "link.on(ev.LINK_CONNECTED,function(e) s.role=e.role end);"                 \
  "link.on(ev.LINK_MESSAGE,function(e) s.msgs[#s.msgs+1]=e.data end);"        \
  "link.on(ev.LINK_DISCONNECTED,function(e) s.disc=e.reason end);"            \
  "link.on(ev.LINK_ERROR,function(e) s.err=e.reason end);"                    \
  "local function wait(f) for _=1,4000 do if f() then return end "            \
  "rt.sleep(5) end error('wait timed out') end;"                              \
  "local function mark() require('capability').call('mark','{}') end;"

static const char s_host_round_trip[] =
    LUA_PRELUDE
    "assert(link.available());"
    "local ok,err=link.send('early');assert(ok==nil and err=='link: not connected');"
    "assert(link.host({tag=args.tag}));"
    "assert(link.state()=='hosting');"
    "local busy,berr=link.join({tag=args.tag});assert(busy==nil and berr=='link: busy');"
    "wait(function() return s.role end);assert(s.role=='host');"
    "assert(link.state()=='connected');"
    "local big=string.rep('\\0\\1\\255',85)..'z';assert(#big==256);"
    "for i=1,3 do assert(link.send('ping-'..i)) end;assert(link.send(big));"
    "wait(function() return #s.msgs==4 end);"
    "for i=1,3 do assert(s.msgs[i]=='echo:ping-'..i,s.msgs[i]) end;"
    "assert(s.msgs[4]==big);"
    "wait(function() return s.disc end);assert(s.disc=='peer_closed',s.disc);"
    "assert(link.state()=='idle');"
    "return 'host-ok'";

static const char s_join_round_trip[] =
    LUA_PRELUDE
    "assert(link.join({tag=args.tag,timeout_ms=5000}));"
    "wait(function() return s.role end);assert(s.role=='join');"
    "wait(function() return #s.msgs==4 end);"
    "for i=1,3 do assert(link.send('echo:'..s.msgs[i])) end;"
    "assert(link.send(s.msgs[4]));"
    "assert(not pcall(link.send,string.rep('x',257)));"
    "assert(not pcall(link.send,''));"
    "rt.sleep(100);"
    "assert(link.close());assert(link.state()=='idle');"
    "return 'join-ok'";

static const char s_host_burst[] =
    LUA_PRELUDE
    "assert(link.host({tag=args.tag}));"
    "wait(function() return s.role end);"
    "local busy=0;local pad=string.rep('p',190);"
    "for i=1,200 do while true do "
    "local ok,err=link.send(string.format('%04d',i)..pad);"
    "if ok then break end;assert(err=='link: busy',err);busy=busy+1;rt.sleep(2) "
    "end end;"
    "assert(busy>0,'sender never saw back-pressure');"
    "wait(function() return s.disc end);assert(s.disc=='peer_closed',s.disc);"
    "return 'burst-ok'";

static const char s_join_burst[] =
    LUA_PRELUDE
    "assert(link.join({tag=args.tag,timeout_ms=5000}));"
    "wait(function() return #s.msgs==200 end);"
    "for i=1,200 do assert(s.msgs[i]:sub(1,4)==string.format('%04d',i),"
    "'out of order at '..i) end;"
    "return 'burst-ok'";

static const char s_wait_lost[] =
    LUA_PRELUDE
    "if args.tag=='lost-host' then assert(link.host({tag='lost'})) "
    "else assert(link.join({tag='lost',timeout_ms=5000})) end;"
    "wait(function() return s.role end);mark();"
    "wait(function() return s.disc end);assert(s.disc=='lost',s.disc);"
    "return 'lost-ok'";

static const char s_connect_then_idle[] =
    LUA_PRELUDE
    "if args.tag=='host' then assert(link.host({tag='idle'})) "
    "else assert(link.join({tag='idle',timeout_ms=5000})) end;"
    "wait(function() return s.role end);mark();"
    "while true do rt.sleep(10) end";

static const char s_wait_peer_closed[] =
    LUA_PRELUDE
    "assert(link.host({tag='idle'}));"
    "wait(function() return s.role end);mark();"
    "wait(function() return s.disc end);assert(s.disc=='peer_closed',s.disc);"
    "return 'peer-closed-ok'";

static const char s_host_forever[] =
    LUA_PRELUDE
    "assert(link.host({tag=args.tag}));mark();"
    "while true do rt.sleep(10) end";

static const char s_join_expect_not_found[] =
    LUA_PRELUDE
    "assert(link.join({tag=args.tag,timeout_ms=300}));"
    "wait(function() return s.err end);assert(s.err=='not_found',s.err);"
    "assert(link.state()=='idle');"
    "return 'not-found-ok'";

static const char s_host_timeout[] =
    LUA_PRELUDE
    "assert(link.host({tag=args.tag,timeout_ms=200}));"
    "wait(function() return s.err end);assert(s.err=='timeout',s.err);"
    "return 'host-timeout-ok'";

static const char s_unavailable[] =
    "local link=require('link');local rt=require('runtime');"
    "assert(link.available()==false);"
    "local ok,err=link.host({tag='x'});assert(ok==nil and err=='link: unavailable');"
    "ok,err=link.join({tag='x'});assert(ok==nil and err=='link: unavailable');"
    "ok,err=link.send('x');assert(ok==nil and err=='link: unavailable');"
    "assert(link.close()==true);assert(link.state()=='unavailable');"
    "local h=link.on(rt.event.LINK_MESSAGE,function() end);"
    "assert(link.off(h)==true);"
    "assert(not pcall(link.on,12345,function() end));"
    "return 'unavailable-ok'";

static const char s_bad_options[] =
    "local link=require('link');"
    "assert(not pcall(link.host,{}));"
    "assert(not pcall(link.host,{tag=''}));"
    "assert(not pcall(link.host,{tag=string.rep('t',33)}));"
    "assert(not pcall(link.join,{tag='t',timeout_ms=0}));"
    "assert(not pcall(link.join,{tag='t',timeout_ms=60001}));"
    "assert(not pcall(link.host,'t'));"
    "return 'options-ok'";

/* ---- Tests ---- */

typedef struct pair {
  fake_air_t air;
  h2_runtime_t *runtime[2];
  h2_lua_host_t *host[2];
} pair_t;

static void pair_open(pair_t *pair) {
  fake_air_init(&pair->air);
  for (int i = 0; i < 2; ++i) {
    atomic_store(&s_marks[i], 0);
    pair->runtime[i] = create_runtime(&pair->air.devices[i].ble,
                                      &pair->air.devices[i].events);
    fake_set_baseline(&pair->air.devices[i]);
    pair->host[i] = create_host(pair->runtime[i], 1, &s_marks[i]);
  }
}

static void pair_close(pair_t *pair) {
  for (int i = 0; i < 2; ++i) {
    if (pair->host[i] != NULL) {
      h2_lua_host_destroy(pair->host[i]);
    }
    assert(fake_is_released(&pair->air.devices[i]));
    h2_runtime_deinit(pair->runtime[i]);
  }
}

static void test_round_trip_and_peer_close(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host = submit(pair.host[0], "@host.lua", s_host_round_trip,
                                "tetris-duel");
  h2_lua_job_id_t join = submit(pair.host[1], "@join.lua", s_join_round_trip,
                                "tetris-duel");
  expect_success(pair.host[1], join, "join-ok");
  expect_success(pair.host[0], host, "host-ok");
  wait_until(device_released, &pair.air.devices[0]);
  wait_until(device_released, &pair.air.devices[1]);
  pair_close(&pair);
}

static void test_flow_control_keeps_order(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@burst.lua", s_host_burst, "burst");
  h2_lua_job_id_t join =
      submit(pair.host[1], "@burst.lua", s_join_burst, "burst");
  expect_success(pair.host[1], join, "burst-ok");
  expect_success(pair.host[0], host, "burst-ok");
  wait_until(device_released, &pair.air.devices[0]);
  wait_until(device_released, &pair.air.devices[1]);
  pair_close(&pair);
}

static void test_link_loss(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@lost.lua", s_wait_lost, "lost-host");
  h2_lua_job_id_t join =
      submit(pair.host[1], "@lost.lua", s_wait_lost, "lost-join");
  wait_until(mark_reached, &s_marks[0]);
  wait_until(mark_reached, &s_marks[1]);
  fake_drop_link(&pair.air);
  expect_success(pair.host[0], host, "lost-ok");
  expect_success(pair.host[1], join, "lost-ok");
  wait_until(device_released, &pair.air.devices[0]);
  wait_until(device_released, &pair.air.devices[1]);
  pair_close(&pair);
}

static void test_job_exit_releases_link(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@peer.lua", s_wait_peer_closed, "host");
  h2_lua_job_id_t join =
      submit(pair.host[1], "@idle.lua", s_connect_then_idle, "join");
  wait_until(mark_reached, &s_marks[0]);
  wait_until(mark_reached, &s_marks[1]);
  /* The joining app exits: its side releases BLE and the host sees BYE. */
  assert(h2_lua_job_cancel(pair.host[1], join) == H2_PAL_OK);
  assert(wait_job(pair.host[1], join).state == H2_LUA_JOB_CANCELLED);
  wait_until(device_released, &pair.air.devices[1]);
  expect_success(pair.host[0], host, "peer-closed-ok");
  wait_until(device_released, &pair.air.devices[0]);
  assert(h2_lua_job_release(pair.host[1], join) == H2_PAL_OK);
  pair_close(&pair);
}

static void test_host_destroy_releases_link(void) {
  pair_t pair;
  pair_open(&pair);
  (void)submit(pair.host[0], "@forever.lua", s_host_forever, "destroy");
  wait_until(mark_reached, &s_marks[0]);
  assert(fake_snapshot(&pair.air.devices[0]).adv_running == 1);
  assert(fake_snapshot(&pair.air.devices[0]).registered == 1);
  /* Destroy joins the session task: nothing is left once it returns. */
  h2_lua_host_destroy(pair.host[0]);
  pair.host[0] = NULL;
  assert(fake_is_released(&pair.air.devices[0]));

  /* A connected session is also torn down by destroy, and the peer sees
   * the BYE. */
  pair.host[0] = create_host(pair.runtime[0], 1, &s_marks[0]);
  atomic_store(&s_marks[0], 0);
  atomic_store(&s_marks[1], 0);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@peer.lua", s_wait_peer_closed, "host");
  (void)submit(pair.host[1], "@idle.lua", s_connect_then_idle, "join");
  wait_until(mark_reached, &s_marks[0]);
  wait_until(mark_reached, &s_marks[1]);
  h2_lua_host_destroy(pair.host[1]);
  pair.host[1] = NULL;
  assert(fake_is_released(&pair.air.devices[1]));
  expect_success(pair.host[0], host, "peer-closed-ok");
  pair_close(&pair);
}

static void test_tag_mismatch_and_timeouts(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@forever.lua", s_host_forever, "alpha");
  wait_until(mark_reached, &s_marks[0]);
  h2_lua_job_id_t join =
      submit(pair.host[1], "@join.lua", s_join_expect_not_found, "beta");
  expect_success(pair.host[1], join, "not-found-ok");
  /* The unrelated joiner never connected; the host keeps advertising. */
  assert(fake_snapshot(&pair.air.devices[0]).adv_running == 1);
  assert(fake_snapshot(&pair.air.devices[1]).connected == 0);
  assert(h2_lua_job_cancel(pair.host[0], host) == H2_PAL_OK);
  (void)wait_job(pair.host[0], host);
  assert(h2_lua_job_release(pair.host[0], host) == H2_PAL_OK);
  wait_until(device_released, &pair.air.devices[0]);

  host = submit(pair.host[0], "@timeout.lua", s_host_timeout, "gamma");
  expect_success(pair.host[0], host, "host-timeout-ok");
  wait_until(device_released, &pair.air.devices[0]);

  h2_lua_job_id_t options =
      submit(pair.host[0], "@options.lua", s_bad_options, "x");
  expect_success(pair.host[0], options, "options-ok");
  pair_close(&pair);
}

static void test_capability_off(void) {
  fake_air_t air;
  const h2_lua_link_config_t link_config = {
      .adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY,
      .scan_type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
  };
  fake_air_init(&air);

  /* BLE present but the launcher did not enable the link. */
  h2_runtime_t *runtime = create_runtime(&air.devices[0].ble,
                                         &air.devices[0].events);
  fake_set_baseline(&air.devices[0]);
  h2_lua_host_t *host = create_host(runtime, 0, NULL);
  expect_success(host, submit(host, "@off.lua", s_unavailable, "x"),
                 "unavailable-ok");
  assert(h2_lua_link_enable(host, &link_config) == H2_PAL_ERR_INVALID_STATE);
  h2_lua_host_destroy(host);
  assert(fake_is_released(&air.devices[0]));
  h2_runtime_deinit(runtime);

  /* A board without BLE: enable reports UNSUPPORTED and link stays off. */
  runtime = create_runtime(h2_pal_unsupported_ble_host_api(),
                           h2_pal_unsupported_system_event_api());
  const h2_lua_host_config_t config = {.runtime = runtime};
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_link_enable(host, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_link_enable(host, &(h2_lua_link_config_t){
                                      .adv_type = (h2_pal_ble_adv_type_t)7,
                                  }) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_link_enable(host, &link_config) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  expect_success(host, submit(host, "@off.lua", s_unavailable, "x"),
                 "unavailable-ok");
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);

  /* BLE present but no system-event API. */
  runtime = create_runtime(&air.devices[0].ble,
                           h2_pal_unsupported_system_event_api());
  const h2_lua_host_config_t no_events = {.runtime = runtime};
  assert(h2_lua_host_create(&no_events, &host) == H2_PAL_OK);
  assert(h2_lua_link_enable(host, &link_config) == H2_PAL_ERR_UNSUPPORTED);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
}

int main(void) {
  test_capability_off();
  test_round_trip_and_peer_close();
  test_flow_control_keeps_order();
  test_link_loss();
  test_job_exit_releases_link();
  test_host_destroy_releases_link();
  test_tag_mismatch_and_timeouts();
  puts("lua link tests passed");
  return 0;
}
