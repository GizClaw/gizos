#include "h2_alipay_tap_radio.h"
#include "h2_app_test_mem.h"
#include <assert.h>
#include <string.h>

typedef struct fixture {
  h2_app_test_mem_t memory;
  const h2_pal_ble_gatt_service_t* service;
  unsigned registered, unregistered, creates, starts, stops, destroys;
  unsigned disconnects, notifications, wakes;
  int registration_rc, data_rc, start_rc, stop_rc, destroy_rc;
  int unregister_rc, disconnect_rc, notify_rc;
} fixture_t;

static int register_service(void* user,
                            const h2_pal_ble_gatt_service_t* service,
                            size_t count) {
  fixture_t* fixture = user;
  assert(count == 1u && service->uuid.len == 2u &&
         service->uuid.data[0] == 2u && service->uuid.data[1] == 0x38u);
  assert(service->characteristic_count == 1u);
  if (fixture->registration_rc != 0) return fixture->registration_rc;
  fixture->service = service;
  *service->characteristics->out_value_handle = 42u;
  ++fixture->registered;
  return 0;
}

static int unregister_service(void* user, const h2_pal_ble_uuid_t* uuid) {
  fixture_t* fixture = user;
  assert(uuid->len == 2u && uuid->data[1] == 0x38u);
  if (fixture->unregister_rc != 0) return fixture->unregister_rc;
  ++fixture->unregistered;
  fixture->service = NULL;
  return 0;
}

static int create(void* user, const h2_pal_ble_adv_params_t* parameters,
                  h2_pal_ble_adv_set_t** out) {
  fixture_t* fixture = user;
  assert(parameters->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE);
  ++fixture->creates;
  *out = (h2_pal_ble_adv_set_t*)fixture;
  return 0;
}

static int set_data(void* user, h2_pal_ble_adv_set_t* set,
                    const h2_pal_ble_adv_data_t* data) {
  fixture_t* fixture = user;
  assert(set == (void*)fixture && data->service_data.len == 6u);
  assert(data->service_data_uuid.len == 2u && data->service_uuid_count == 1u);
  return fixture->data_rc;
}

static int start(void* user, h2_pal_ble_adv_set_t* set) {
  fixture_t* fixture = user;
  assert(set == (void*)fixture);
  if (fixture->start_rc != 0) return fixture->start_rc;
  ++fixture->starts;
  return 0;
}

static int stop(void* user, h2_pal_ble_adv_set_t* set) {
  fixture_t* fixture = user;
  assert(set == (void*)fixture);
  if (fixture->stop_rc != 0) return fixture->stop_rc;
  ++fixture->stops;
  return 0;
}

static int destroy(void* user, h2_pal_ble_adv_set_t* set) {
  fixture_t* fixture = user;
  assert(set == (void*)fixture);
  if (fixture->destroy_rc != 0) return fixture->destroy_rc;
  ++fixture->destroys;
  return 0;
}

static int disconnect(void* user, uint16_t connection) {
  fixture_t* fixture = user;
  assert(connection == 7u);
  if (fixture->disconnect_rc != 0) return fixture->disconnect_rc;
  ++fixture->disconnects;
  return 0;
}

static int notify(void* user, uint16_t connection, uint16_t handle,
                  const uint8_t* data, size_t length) {
  fixture_t* fixture = user;
  assert(connection == 7u && handle == 42u && data[0] == 0x42u &&
         length == 20u);
  ++fixture->notifications;
  return fixture->notify_rc;
}

static void wake(void* user) { ++((fixture_t*)user)->wakes; }

static int write(fixture_t* fixture, uint16_t connection, uint16_t offset) {
  const uint8_t data[20] = {0x42u};
  const h2_pal_ble_gatt_access_t access = {
      .conn_handle = connection, .attr_handle = 42u, .offset = offset};
  return fixture->service->characteristics->write(
      fixture->service->characteristics->user, &access, data, sizeof(data));
}

int main(void) {
  fixture_t fixture = {0};
  h2_app_test_mem_init(&fixture.memory, NULL);
  const h2_pal_ble_vtable_t vtable = {
      .register_gatt_services = register_service,
      .unregister_gatt_service = unregister_service,
      .adv_set_create = create,
      .adv_set_set_data = set_data,
      .adv_set_start = start,
      .adv_set_stop = stop,
      .adv_set_destroy = destroy,
      .notify = notify,
      .disconnect = disconnect};
  const h2_pal_ble_host_api_t ble = {.user = &fixture, .vtable = &vtable};
  const h2_alipay_tap_radio_config_t config = {.memory = &fixture.memory.api,
                                               .ble = &ble,
                                               .mac = {1u, 2u, 3u, 4u, 5u, 6u},
                                               .user = &fixture,
                                               .wake = wake};
  h2_alipay_tap_radio_t* radio = NULL;
  fixture.memory.fail_at = fixture.memory.calls + 1u;
  assert(h2_alipay_tap_radio_open(&config, &radio) == H2_PAL_ERR_NO_MEMORY);
  assert(radio == NULL && fixture.memory.live_blocks == 0u);
  fixture.memory.fail_at = 0u;
  fixture.registration_rc = H2_PAL_ERR_NO_SPACE;
  assert(h2_alipay_tap_radio_open(&config, &radio) == H2_PAL_ERR_NO_SPACE);
  assert(radio == NULL && fixture.memory.live_blocks == 0u);
  fixture.registration_rc = 0;
  fixture.data_rc = H2_PAL_ERR_IO;
  assert(h2_alipay_tap_radio_open(&config, &radio) == H2_PAL_ERR_IO);
  assert(radio == NULL && fixture.memory.live_blocks == 0u);
  fixture.data_rc = 0;
  fixture.start_rc = H2_PAL_ERR_IO;
  fixture.unregister_rc = H2_PAL_ERR_BUSY;
  assert(h2_alipay_tap_radio_open(&config, &radio) == H2_PAL_ERR_IO);
  assert(radio != NULL && fixture.memory.live_blocks != 0u);
  assert(write(&fixture, 7u, 0u) == H2_PAL_ERR_CLOSED);
  fixture.unregister_rc = 0;
  assert(h2_alipay_tap_radio_close(&radio) == 0);
  assert(radio == NULL && fixture.memory.live_blocks == 0u);
  fixture.start_rc = 0;
  assert(h2_alipay_tap_radio_open(&config, &radio) == 0);
  assert(write(&fixture, 7u, 0u) == H2_PAL_ERR_CLOSED);
  assert(h2_alipay_tap_radio_arm(radio) == 0);
  assert(write(&fixture, 7u, 1u) == H2_PAL_ERR_INVALID_ARG);
  for (unsigned index = 0u; index < 8u; ++index)
    assert(write(&fixture, 7u, 0u) == 0);
  assert(write(&fixture, 8u, 0u) == H2_PAL_ERR_BUSY);
  assert(write(&fixture, 7u, 0u) == H2_PAL_ERR_NO_SPACE);
  assert(fixture.wakes == 8u);
  uint8_t output[20] = {0};
  size_t length = 19u;
  assert(h2_alipay_tap_radio_take(radio, output, &length) ==
         H2_PAL_ERR_NO_SPACE);
  assert(length == 20u && output[0] == 0u);
  for (unsigned index = 0u; index < 8u; ++index) {
    length = sizeof(output);
    assert(h2_alipay_tap_radio_take(radio, output, &length) == 0);
    assert(length == 20u && output[0] == 0x42u);
  }
  assert(h2_alipay_tap_radio_take(radio, output, &length) ==
         H2_PAL_ERR_WOULD_BLOCK);
  fixture.notify_rc = H2_PAL_ERR_IO;
  assert(h2_alipay_tap_radio_notify(radio, false, output, 20u) ==
         H2_PAL_ERR_IO);
  fixture.notify_rc = 0;
  assert(h2_alipay_tap_radio_notify(radio, true, output, 20u) == 0);
  assert(write(&fixture, 7u, 0u) == H2_PAL_ERR_CLOSED);
  fixture.stop_rc = H2_PAL_ERR_BUSY;
  assert(h2_alipay_tap_radio_close(&radio) == H2_PAL_ERR_BUSY);
  assert(radio != NULL && fixture.memory.live_blocks == 1u);
  fixture.stop_rc = 0;
  fixture.destroy_rc = H2_PAL_ERR_BUSY;
  assert(h2_alipay_tap_radio_close(&radio) == H2_PAL_ERR_BUSY);
  fixture.destroy_rc = 0;
  fixture.unregister_rc = H2_PAL_ERR_BUSY;
  assert(h2_alipay_tap_radio_close(&radio) == H2_PAL_ERR_BUSY);
  assert(write(&fixture, 7u, 0u) == H2_PAL_ERR_CLOSED);
  fixture.unregister_rc = 0;
  fixture.disconnect_rc = H2_PAL_ERR_BUSY;
  assert(h2_alipay_tap_radio_close(&radio) == H2_PAL_ERR_BUSY);
  fixture.disconnect_rc = 0;
  assert(h2_alipay_tap_radio_close(&radio) == 0);
  assert(radio == NULL && fixture.memory.live_blocks == 0u);
  assert(fixture.registered == fixture.unregistered &&
         fixture.creates == fixture.destroys &&
         fixture.starts == fixture.stops && fixture.disconnects == 1u);
  return 0;
}
