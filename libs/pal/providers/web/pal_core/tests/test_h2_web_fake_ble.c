#include "h2_web_main_thread.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      fprintf(stderr, "fake BLE line %d: %s\n", __LINE__, #value);             \
      emscripten_force_exit(1);                                                \
    }                                                                          \
  } while (0)

static _Atomic int s_client;
static _Atomic unsigned s_writes;
static _Atomic bool s_block_started, s_block_finished;
static _Atomic unsigned s_connections, s_subscriptions, s_disconnections;
static const h2_pal_ble_api_t *s_ble;
static uint16_t s_value;

EMSCRIPTEN_KEEPALIVE void fake_ble_test_client_result(int result) {
  s_client = result;
}

/* clang-format off */
EM_JS(void, configure_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32'], null, mode => {
    delete Module.h2WebEnvironment;
    if (mode) Module.h2WebEnvironment = {version: 1,
      wifi: {enabled:true, connected:true, ssid:'Browser Wi-Fi', rssi:-48},
      modem: {enabled:false, simPresent:false, registered:false, rssi:-70},
      ble: {enabled: mode === 1 ? true : 'invalid'}};
  });
});
EM_JS(void, client_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32'], null, handle => {
    const central = Module.h2WebBluetooth;
    const check = value => { if (!value) throw new Error('simulated central assertion'); };
    const rejects = async (call, code) => {
      try { await call(); } catch (error) { check(error.code === code); return; }
      throw new Error('simulated ATT unexpectedly succeeded');
    };
    (async () => {
      check(central.simulated && central.scan().length === 1);
      check(central.scan()[0].localName === 'Web BLE test');
      check(central.scan()[0].advertisement.serviceUuids[0].join() === '52,18');
      check(central.snapshot.services.length === 1);
      await central.connect(); check(central.snapshot.connected && !central.snapshot.advertising);
      await rejects(() => central.connect(), -7);
      await rejects(() => central.write(65534, Uint8Array.of(1)), -8);
      await rejects(() => central.write(handle, new Uint8Array(9)), -1);
      await rejects(() => central.write(handle, Uint8Array.of(0)), -15);
      await central.subscribe(handle);
      await central.write(handle, Uint8Array.of(7,8));
      const read = await central.read(handle); check(read.value.join() === '111,107');
      check(central.notifications.at(-1).value.join() === '7,8');
      await central.subscribe(handle, false);
      await central.disconnect(); check(!central.snapshot.connected);
      await rejects(() => central.write(handle, Uint8Array.of(7)), -8);
      globalThis.retainedTestCentral = central;
      Module._fake_ble_test_client_result(1);
    })().catch(error => { console.error(error); Module._fake_ble_test_client_result(-1); });
  });
});
EM_JS(void, closed_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], null, () => {
    if (Module.h2WebBluetooth) { Module._fake_ble_test_client_result(-1); return; }
    globalThis.retainedTestCentral.connect().then(
      () => Module._fake_ble_test_client_result(-1),
      error => Module._fake_ble_test_client_result(error.code === -10 ? 2 : -1));
  });
});
EM_JS(void, fence_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32'], null, handle => {
    (async () => {
      await globalThis.retainedTestCentral.connect();
      await globalThis.retainedTestCentral.write(handle, Uint8Array.of(9));
      Module._fake_ble_test_client_result(3);
    })().catch(error => { console.error(error); Module._fake_ble_test_client_result(-1); });
  });
});
/* clang-format on */

static int event(void *user, const h2_pal_system_event_t *value) {
  (void)user;
  if (value->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED) {
    const h2_pal_ble_connection_t *connection = value->payload;
    CHECK(connection->role == H2_PAL_BLE_ROLE_PERIPHERAL);
    CHECK(connection->mtu == H2_PAL_BLE_ATT_MAX_MTU);
    ++s_connections;
  } else if (value->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED) {
    ++s_subscriptions;
  } else if (value->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED) {
    ++s_disconnections;
  }
  return H2_PAL_OK;
}

static int write_value(void *user, const h2_pal_ble_gatt_access_t *access,
                       const uint8_t *data, size_t len) {
  (void)user;
  if (len == 1 && data[0] == 9) {
    s_block_started = true;
    emscripten_thread_sleep(30);
    s_block_finished = true;
    return H2_PAL_OK;
  }
  if (len != 2 || data[0] != 7 || data[1] != 8)
    return H2_PAL_ERR_FORMAT;
  CHECK(h2_pal_ble_unregister_gatt_services(s_ble) == H2_PAL_ERR_BUSY);
  CHECK(h2_pal_ble_notify(s_ble, access->conn_handle, access->attr_handle, data,
                          len) == H2_PAL_OK);
  ++s_writes;
  return H2_PAL_OK;
}

static int read_value(void *user, const h2_pal_ble_gatt_access_t *access,
                      uint8_t *out, size_t size, size_t *len) {
  (void)user;
  (void)access;
  CHECK(size >= 2);
  memcpy(out, "ok", 2);
  *len = 2;
  return H2_PAL_OK;
}

static int destroy(h2_web_platform_t *p) {
  int rc = H2_PAL_ERR_BUSY;
  for (unsigned i = 0; i < 1000 && rc == H2_PAL_ERR_BUSY; ++i) {
    rc = h2_web_platform_destroy(p);
    if (rc == H2_PAL_ERR_BUSY)
      emscripten_thread_sleep(1);
  }
  return rc;
}

int main(void) {
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  int mode = 0;
  h2_web_main_call(configure_js, (const void *[]){&mode});
  h2_web_platform_t *p = h2_web_platform_create(&config);
  CHECK(p && !h2_web_platform_fake_ble_api(p));
  CHECK(destroy(p) == H2_PAL_OK);
  mode = 2;
  h2_web_main_call(configure_js, (const void *[]){&mode});
  CHECK(!h2_web_platform_create(&config));
  mode = 1;
  h2_web_main_call(configure_js, (const void *[]){&mode});
  p = h2_web_platform_create(&config);
  CHECK(p);
  s_ble = h2_web_platform_fake_ble_api(p);
  CHECK(s_ble);
  CHECK(h2_pal_ble_start(s_ble) == H2_PAL_OK);
  const uint8_t uuid_bytes[] = {0x34, 0x12};
  const h2_pal_ble_uuid_t uuid = {.data = uuid_bytes,
                                  .len = sizeof(uuid_bytes)};
  uint16_t cccd = 0;
  const h2_pal_ble_gatt_characteristic_t characteristic = {
      .uuid = uuid,
      .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE |
                    H2_PAL_BLE_GATT_PROPERTY_READ |
                    H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
      .permissions =
          H2_PAL_BLE_GATT_PERMISSION_WRITE | H2_PAL_BLE_GATT_PERMISSION_READ,
      .max_value_len = 8,
      .write = write_value,
      .read = read_value,
      .out_value_handle = &s_value,
      .out_cccd_handle = &cccd};
  const h2_pal_ble_gatt_service_t service = {.uuid = uuid,
                                             .primary = true,
                                             .characteristics = &characteristic,
                                             .characteristic_count = 1};
  CHECK(h2_pal_ble_register_gatt_services(s_ble, &service, 1) == H2_PAL_OK);
  CHECK(s_value && cccd);
  CHECK(h2_pal_ble_register_gatt_services(s_ble, &service, 1) ==
        H2_PAL_ERR_BUSY);
  const h2_pal_ble_adv_data_t advertising = {.local_name = "Web BLE test",
                                             .service_uuids = &uuid,
                                             .service_uuid_count = 1};
  const h2_pal_ble_adv_params_t params = {.mode =
                                              H2_PAL_BLE_ADV_MODE_CONNECTABLE,
                                          .interval_min_ms = 100,
                                          .interval_max_ms = 150};
  CHECK(h2_pal_ble_set_adv_data(s_ble, &advertising) == H2_PAL_OK);
  CHECK(h2_pal_ble_start_advertising(s_ble, &params) == H2_PAL_OK);
  const h2_pal_system_event_api_t *events = h2_web_platform_system_event_api(p);
  h2_pal_system_event_subscription_t *subscriptions[3] = {0};
  const h2_pal_system_event_type_t types[] = {
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED};
  for (unsigned i = 0; i < 3; ++i)
    CHECK(h2_pal_system_event_subscribe(events, types[i], event, NULL,
                                        &subscriptions[i]) == H2_PAL_OK);
  int handle = s_value;
  h2_web_main_call(client_js, (const void *[]){&handle});
  for (unsigned i = 0; i < 10000 && !s_client; ++i)
    emscripten_thread_sleep(1);
  CHECK(s_client == 1 && s_writes == 1 && s_connections == 1 &&
        s_subscriptions == 2 && s_disconnections == 1);
  CHECK(h2_pal_ble_start_advertising(s_ble, &params) == H2_PAL_OK);
  h2_web_main_call(fence_js, (const void *[]){&handle});
  for (unsigned i = 0; i < 1000 && !s_block_started; ++i)
    emscripten_thread_sleep(1);
  CHECK(s_block_started);
  CHECK(h2_pal_ble_unregister_gatt_service(s_ble, &uuid) == H2_PAL_OK);
  CHECK(s_block_finished);
  for (unsigned i = 0; i < 1000 && s_client == 1; ++i)
    emscripten_thread_sleep(1);
  CHECK(s_client == 3);
  CHECK(h2_pal_ble_unregister_gatt_service(s_ble, &uuid) ==
        H2_PAL_ERR_NOT_FOUND);
  CHECK(h2_pal_ble_stop(s_ble) == H2_PAL_OK);
  for (unsigned i = 0; i < 3; ++i)
    h2_pal_system_event_unsubscribe(events, subscriptions[i]);
  CHECK(destroy(p) == H2_PAL_OK);
  s_client = 1;
  h2_web_main_call(closed_js, NULL);
  for (unsigned i = 0; i < 1000 && s_client == 1; ++i)
    emscripten_thread_sleep(1);
  CHECK(s_client == 2);
  puts("Web simulated BLE: ATT callbacks, discovery, notifications, errors and "
       "cleanup PASS");
  return 0;
}
