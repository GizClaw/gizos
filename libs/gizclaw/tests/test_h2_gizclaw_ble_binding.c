#include "h2_desktop_platform.h"
#include "h2_gizclaw_ble_binding.h"
#include "h2_gizclaw_service_internal.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "yyjson.h"
#include <stdlib.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_atomic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Use the same client/request seams as the Service tests. The real Service
 * worker, request queues, cancellation and owner dispatch remain under test. */
typedef struct test_env {
  h2_gizclaw_service_t *service;
  h2_gizclaw_api_key_state_t *state;
  h2_gizclaw_cancel_fn cancel;
  void *cancel_user;
  h2_atomic_bool_t connected;
  h2_atomic_bool_t reply;
  h2_atomic_uint_t starts;
  h2_atomic_uint_t destroys;
  h2_atomic_size_t now;
  int methods[128];
  char secret[96];
  char key_name[27];
  int method;
  h2_pal_result_t revoke_result;
  h2_pal_result_t create_result;
  bool create_rpc_error, revoke_rpc_error;
  int create_rpc_code, revoke_rpc_code;
  h2_pal_time_api_t time;
} test_env_t;
static test_env_t *s_env;

/* Real time bounds scheduling waits; env->time remains the fake RPC clock. */
static uint64_t wall_ms(void) {
  uint64_t now;
  assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &now) ==
         H2_PAL_OK);
  return now;
}
static void pause_poll(void) {
  assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) == H2_PAL_OK);
}

static h2_pal_result_t fake_init(const h2_gizclaw_config_t *config,
                                 h2_gizclaw_client_t **out_client) {
  s_env->cancel = config->cancel_requested;
  s_env->cancel_user = config->cancel_user;
  *out_client = (h2_gizclaw_client_t *)s_env;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_connect(h2_gizclaw_client_t *client) {
  (void)client;
  const uint64_t deadline = wall_ms() + 10000u;
  while (!h2_atomic_load(&s_env->connected)) {
    assert(wall_ms() < deadline);
    if (s_env->cancel(s_env->cancel_user))
      return H2_PAL_ERR_CLOSED;
    pause_poll();
  }
  return H2_PAL_OK;
}
static h2_pal_result_t fake_poll(h2_gizclaw_client_t *client, int timeout_ms) {
  (void)client;
  (void)timeout_ms;
  return H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t fake_handler(h2_gizclaw_client_t *client,
                                    h2_gizclaw_client_event_sink_fn handler,
                                    void *user) {
  (void)client;
  (void)handler;
  (void)user;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_dispatch(h2_gizclaw_client_t *client) {
  (void)client;
  return H2_PAL_ERR_WOULD_BLOCK;
}
static h2_pal_result_t fake_close(h2_gizclaw_client_t *client) {
  (void)client;
  return H2_PAL_OK;
}
static void fake_deinit(h2_gizclaw_client_t *client) { (void)client; }
static const h2_gizclaw_service_client_ops_t client_ops = {
    .init = fake_init,
    .connect = fake_connect,
    .poll = fake_poll,
    .set_event_handler = fake_handler,
    .dispatch_event = fake_dispatch,
    .close = fake_close,
    .deinit = fake_deinit};

static int fake_start(h2_gizclaw_client_t *client,
                      h2_gizclaw_rpc_method_t method,
                      h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
                      h2_gizclaw_rpc_request_t **out_request) {
  (void)client;
  assert(timeout_ms != 0u);
  pb_istream_t stream = pb_istream_from_buffer(payload.data, payload.len);
  if (method == 96) {
    gizclaw_rpc_v1_APIKeyCreateRequest message = {0};
    assert(pb_decode(&stream, gizclaw_rpc_v1_APIKeyCreateRequest_fields,
                     &message));
    assert(strcmp(message.display_name, "binding") == 0);
    assert(message.manage_api_keys);
  } else {
    assert(method == 98);
    gizclaw_rpc_v1_APIKeyRevokeRequest message = {0};
    assert(pb_decode(&stream, gizclaw_rpc_v1_APIKeyRevokeRequest_fields,
                     &message));
    assert(message.name[0] != 0);
  }
  s_env->method = method;
  unsigned count = h2_atomic_load(&s_env->starts);
  assert(count < 128u);
  s_env->methods[count] = method;
  *out_request = (h2_gizclaw_rpc_request_t *)s_env;
  h2_atomic_fetch_add(&s_env->starts, 1u);
  return H2_PAL_OK;
}
static int fake_result(h2_gizclaw_rpc_request_t *request,
                       h2_gizclaw_rpc_response_t *out_response) {
  (void)request;
  if (!h2_atomic_load(&s_env->reply))
    return H2_PAL_ERR_WOULD_BLOCK;
  int rc = s_env->method == 98 ? s_env->revoke_result : s_env->create_result;
  *out_response = (h2_gizclaw_rpc_response_t){
      .has_error = s_env->method == 98 ? s_env->revoke_rpc_error
                                       : s_env->create_rpc_error,
      .error_code = s_env->method == 98 ? s_env->revoke_rpc_code
                                        : s_env->create_rpc_code};
  if (rc != H2_PAL_OK)
    return rc;
  if (out_response->has_error)
    return H2_PAL_OK;
  uint8_t bytes[256];
  pb_ostream_t stream = pb_ostream_from_buffer(bytes, sizeof(bytes));
  if (s_env->method == 96) {
    gizclaw_rpc_v1_APIKeyCreateResponse message = {.has_value = true};
    strcpy(message.value.name, s_env->key_name);
    strcpy(message.api_key, s_env->secret);
    assert(pb_encode(&stream, gizclaw_rpc_v1_APIKeyCreateResponse_fields,
                     &message));
  } else {
    gizclaw_rpc_v1_APIKeyRevokeResponse message = {0};
    assert(pb_encode(&stream, gizclaw_rpc_v1_APIKeyRevokeResponse_fields,
                     &message));
  }
  *out_response = (h2_gizclaw_rpc_response_t){0};
  if (stream.bytes_written != 0u) {
    out_response->result_payload = h2_pal_mem_alloc(
        h2_desktop_platform_default_allocator(), stream.bytes_written);
    assert(out_response->result_payload != NULL);
    memcpy(out_response->result_payload, bytes, stream.bytes_written);
    out_response->result_payload_len = stream.bytes_written;
  }
  return H2_PAL_OK;
}
static void fake_cancel(h2_gizclaw_rpc_request_t *request) { (void)request; }
static void fake_destroy(h2_gizclaw_rpc_request_t *request) {
  (void)request;
  h2_atomic_fetch_add(&s_env->destroys, 1u);
}
static const h2_gizclaw_async_rpc_ops_t rpc_ops = {.start = fake_start,
                                                   .result = fake_result,
                                                   .cancel = fake_cancel,
                                                   .destroy = fake_destroy};
static h2_pal_result_t fake_time(void *user, uint64_t *out_ms) {
  test_env_t *env = user;
  *out_ms = h2_atomic_load(&env->now);
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_ops = {.get_monotonic_ms = fake_time};

static void setup(test_env_t *env, bool connected) {
  memset(env, 0, sizeof(*env));
  strcpy(env->secret, "gizclaw_sk_v1_public_fixture_001");
  strcpy(env->key_name, "key-name");
  assert(h2_atomic_bool_init(&env->connected, connected) == H2_ATOMIC_OK);
  assert(h2_atomic_bool_init(&env->reply, true) == H2_ATOMIC_OK);
  assert(h2_atomic_uint_init(&env->starts, 0u) == H2_ATOMIC_OK);
  assert(h2_atomic_uint_init(&env->destroys, 0u) == H2_ATOMIC_OK);
  assert(h2_atomic_size_init(&env->now, 100u) == H2_ATOMIC_OK);
  s_env = env;
  h2_gizclaw_service_test_set_client_ops(&client_ops);
  h2_gizclaw_async_rpc_test_set_ops(&rpc_ops);
  h2_gizclaw_config_t client_config = {
      .allocator = h2_desktop_platform_default_allocator(),
      .time = h2_desktop_platform_time_api()};
  h2_gizclaw_service_config_t config = {.client_config = &client_config,
                                        .task = h2_desktop_platform_task_api(),
                                        .queue =
                                            h2_desktop_platform_queue_api(),
                                        .sync = h2_desktop_platform_sync_api(),
                                        .operation_capacity = 8u,
                                        .client_poll_timeout_ms = 1};
  assert(h2_gizclaw_service_init(&config, &env->service) == H2_PAL_OK);
  assert(h2_gizclaw_service_start(env->service) == H2_PAL_OK);
  env->time = (h2_pal_time_api_t){.user = env, .vtable = &time_ops};
  char name[] = "binding";
  h2_gizclaw_api_key_state_config_t state_config = {
      .service = env->service,
      .mem = client_config.allocator,
      .sync = config.sync,
      .time = &env->time,
      .display_name = {name, strlen(name)},
      .manage_api_keys = true,
      .timeout_ms = 1000u};
  assert(h2_gizclaw_api_key_state_create(&state_config, &env->state) ==
         H2_PAL_OK);
  memset(name, 'x', sizeof(name)); /* State must own its display name. */
}
static h2_gizclaw_api_key_snapshot_t snapshot(test_env_t *env) {
  h2_gizclaw_api_key_snapshot_t result;
  assert(h2_gizclaw_api_key_state_snapshot(env->state, &result) == H2_PAL_OK);
  return result;
}
static void poll_once(test_env_t *env) {
  size_t count = 0u;
  assert(h2_gizclaw_service_poll(env->service, 1u, &count) == H2_PAL_OK);
  pause_poll();
}
static void finish(test_env_t *env) {
  const uint64_t deadline = wall_ms() + 10000u;
  do {
    poll_once(env);
    if (!snapshot(env).busy)
      return;
  } while (wall_ms() < deadline);
  assert(false);
}
static void wait_count(h2_atomic_uint_t *count, unsigned expected) {
  const uint64_t deadline = wall_ms() + 10000u;
  do {
    if (h2_atomic_load(count) >= expected)
      return;
    pause_poll();
  } while (wall_ms() < deadline);
  assert(false);
}
static void destroy_env_atomics(test_env_t *env) {
  h2_atomic_bool_destroy(&env->connected);
  h2_atomic_bool_destroy(&env->reply);
  h2_atomic_uint_destroy(&env->starts);
  h2_atomic_uint_destroy(&env->destroys);
  h2_atomic_size_destroy(&env->now);
}

static void teardown(test_env_t *env) {
  assert(h2_gizclaw_api_key_state_close(env->state) == H2_PAL_OK);
  assert(h2_gizclaw_service_stop(env->service) == H2_PAL_OK);
  const uint64_t deadline = wall_ms() + 10000u;
  int rc;
  do {
    poll_once(env);
    rc = h2_gizclaw_api_key_state_destroy(&env->state);
    if (rc != H2_PAL_ERR_BUSY)
      break;
  } while (wall_ms() < deadline);
  assert(rc == H2_PAL_OK);
  assert(env->state == NULL);
  assert(h2_gizclaw_api_key_state_destroy(&env->state) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(env->service) == H2_PAL_OK);
  destroy_env_atomics(env);
}
static void refresh(test_env_t *env, bool revoke) {
  assert(h2_gizclaw_api_key_state_request_refresh(env->state, revoke) ==
         H2_PAL_OK);
  finish(env);
}

struct h2_pal_ble_adv_set {
  bool live;
};
struct h2_pal_system_event_subscription {
  h2_pal_system_event_type_t type;
  h2_pal_system_event_handler_t handler;
  void *user;
  bool live;
};
typedef struct fake_ble {
  h2_pal_ble_host_api_t api;
  h2_pal_system_event_api_t events;
  struct h2_pal_system_event_subscription subscriptions[6];
  struct h2_pal_ble_adv_set set;
  const h2_pal_ble_gatt_service_t *service;
  bool other_service;
  bool other_advertising;
  unsigned registrations, unregistrations, creates, starts, stops, destroys;
  unsigned subscriptions_created, subscriptions_destroyed;
  h2_pal_result_t create_result, start_result, stop_result, destroy_result;
  h2_pal_result_t register_result, unregister_result;
  unsigned fail_subscribe_at;
  bool read_during_unregister;
} fake_ble_t;

static bool uuid_equal(const h2_pal_ble_uuid_t *a, const h2_pal_ble_uuid_t *b) {
  return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}
static void emit(fake_ble_t *ble, h2_pal_system_event_type_t type,
                 const void *payload, size_t payload_size) {
  h2_pal_system_event_t event = {
      .type = type, .payload = payload, .payload_size = payload_size};
  for (size_t i = 0u; i < 6u; ++i) {
    struct h2_pal_system_event_subscription *sub = &ble->subscriptions[i];
    if (sub->live && sub->type == type)
      assert(sub->handler(sub->user, &event) == H2_PAL_OK);
  }
}
static int fake_subscribe(void *user, h2_pal_system_event_type_t type,
                          h2_pal_system_event_handler_t handler, void *context,
                          h2_pal_system_event_subscription_t **out) {
  fake_ble_t *ble = user;
  if (ble->fail_subscribe_at != 0u &&
      ble->subscriptions_created + 1u == ble->fail_subscribe_at)
    return H2_PAL_ERR_IO;
  for (size_t i = 0u; i < 6u; ++i) {
    if (!ble->subscriptions[i].live) {
      ble->subscriptions[i] = (struct h2_pal_system_event_subscription){
          .type = type, .handler = handler, .user = context, .live = true};
      *out = &ble->subscriptions[i];
      ++ble->subscriptions_created;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NO_SPACE;
}
static void fake_unsubscribe(void *user,
                             h2_pal_system_event_subscription_t *subscription) {
  fake_ble_t *ble = user;
  assert(subscription->live);
  subscription->live = false;
  ++ble->subscriptions_destroyed;
}
static const h2_pal_system_event_vtable_t event_ops = {
    .subscribe = fake_subscribe, .unsubscribe = fake_unsubscribe};
static h2_pal_result_t fake_register(void *user,
                                     const h2_pal_ble_gatt_service_t *services,
                                     size_t count) {
  fake_ble_t *ble = user;
  assert(count == 1u && ble->service == NULL);
  assert(uuid_equal(&services->uuid, &h2_gizclaw_ble_binding_service_uuid));
  assert(services->primary && services->characteristic_count == 3u);
  assert(services->characteristics[0].properties ==
         H2_PAL_BLE_GATT_PROPERTY_READ);
  assert(services->characteristics[1].properties ==
         H2_PAL_BLE_GATT_PROPERTY_WRITE);
  assert(services->characteristics[2].properties ==
         H2_PAL_BLE_GATT_PROPERTY_READ);
  ++ble->registrations;
  if (ble->register_result != H2_PAL_OK)
    return ble->register_result;
  ble->service = services;
  for (size_t i = 0u; i < 3u; ++i)
    *services->characteristics[i].out_value_handle = (uint16_t)(0x10u + i);
  return H2_PAL_OK;
}
static h2_pal_result_t fake_unregister(void *user,
                                       const h2_pal_ble_uuid_t *uuid) {
  fake_ble_t *ble = user;
  assert(uuid_equal(uuid, &h2_gizclaw_ble_binding_service_uuid));
  assert(ble->service != NULL);
  ++ble->unregistrations;
  if (ble->read_during_unregister) {
    const h2_pal_ble_gatt_characteristic_t *info =
        &ble->service->characteristics[0];
    h2_pal_ble_gatt_access_t access = {.conn_handle = 1u, .attr_handle = 0x10u};
    uint8_t out[14];
    size_t len = 99u;
    assert(info->read(info->user, &access, out, sizeof(out), &len) ==
           H2_PAL_ERR_CLOSED);
    assert(len == 0u);
  }
  if (ble->unregister_result != H2_PAL_OK)
    return ble->unregister_result;
  ble->service = NULL;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_set_create(void *user,
                                       const h2_pal_ble_adv_params_t *params,
                                       h2_pal_ble_adv_set_t **out) {
  fake_ble_t *ble = user;
  assert(!ble->set.live);
  assert(params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE);
  assert(params->type == H2_PAL_BLE_ADV_TYPE_LEGACY);
  assert(params->interval_min_ms == 100u && params->interval_max_ms == 200u);
  ++ble->creates;
  if (ble->create_result != H2_PAL_OK)
    return ble->create_result;
  ble->set.live = true;
  *out = &ble->set;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_set_data(void *user, h2_pal_ble_adv_set_t *set,
                                     const h2_pal_ble_adv_data_t *data) {
  fake_ble_t *ble = user;
  assert(set == &ble->set && set->live);
  assert(data->local_name == NULL && data->service_uuid_count == 1u);
  assert(uuid_equal(data->service_uuids, &h2_gizclaw_ble_binding_service_uuid));
  assert(data->manufacturer_data.len == 0u && data->service_data.len == 0u);
  assert(data->service_data_uuid.len == 0u);
  return H2_PAL_OK;
}
static h2_pal_result_t fake_set_start(void *user, h2_pal_ble_adv_set_t *set) {
  fake_ble_t *ble = user;
  assert(set == &ble->set && set->live);
  ++ble->starts;
  h2_pal_ble_adv_set_event_t event = {.set = set, .status = ble->start_result};
  emit(ble, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED, &event,
       sizeof(event));
  return ble->start_result;
}
static h2_pal_result_t fake_set_stop(void *user, h2_pal_ble_adv_set_t *set) {
  fake_ble_t *ble = user;
  assert(set == &ble->set && set->live);
  ++ble->stops;
  h2_pal_ble_adv_set_event_t event = {.set = set, .status = ble->stop_result};
  emit(ble, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, &event,
       sizeof(event));
  return ble->stop_result;
}
static h2_pal_result_t fake_set_destroy(void *user, h2_pal_ble_adv_set_t *set) {
  fake_ble_t *ble = user;
  assert(set == &ble->set && set->live);
  ++ble->destroys;
  if (ble->destroy_result == H2_PAL_OK)
    set->live = false;
  return ble->destroy_result;
}
/* Unrelated/global operations are intentionally absent: any fallback fails. */
static const h2_pal_ble_vtable_t ble_ops = {
    .adv_set_create = fake_set_create,
    .adv_set_set_data = fake_set_data,
    .adv_set_start = fake_set_start,
    .adv_set_stop = fake_set_stop,
    .adv_set_destroy = fake_set_destroy,
    .register_gatt_services = fake_register,
    .unregister_gatt_service = fake_unregister};
static void ble_setup(fake_ble_t *ble) {
  memset(ble, 0, sizeof(*ble));
  ble->api = (h2_pal_ble_host_api_t){.user = ble, .vtable = &ble_ops};
  ble->events = (h2_pal_system_event_api_t){.user = ble, .vtable = &event_ops};
  ble->other_service = ble->other_advertising = true;
}
static h2_gizclaw_str_t text(const char *value) {
  return (h2_gizclaw_str_t){value, strlen(value)};
}
static h2_gizclaw_ble_binding_t *binding_open(test_env_t *env,
                                              fake_ble_t *ble) {
  h2_gizclaw_ble_binding_config_t config = {
      .api_key_state = env->state,
      .ble = &ble->api,
      .system_event = &ble->events,
      .mem = h2_desktop_platform_default_allocator(),
      .sync = h2_desktop_platform_sync_api(),
      .server_origin = text("https://ap.gizclaw.com"),
      .icon = text("h106_tiga"),
      .name = text("客厅")};
  h2_gizclaw_ble_binding_t *binding = NULL;
  assert(h2_gizclaw_ble_binding_open(&config, &binding) == H2_PAL_OK);
  return binding;
}
static h2_gizclaw_ble_binding_snapshot_t
binding_snapshot(h2_gizclaw_ble_binding_t *binding) {
  h2_gizclaw_ble_binding_snapshot_t value;
  assert(h2_gizclaw_ble_binding_snapshot(binding, &value) == H2_PAL_OK);
  return value;
}
static h2_pal_result_t read_value(fake_ble_t *ble, size_t index, uint16_t conn,
                                  uint16_t offset, uint8_t *out,
                                  size_t capacity, size_t *out_len) {
  assert(ble->service != NULL);
  const h2_pal_ble_gatt_characteristic_t *value =
      &ble->service->characteristics[index];
  h2_pal_ble_gatt_access_t access = {.conn_handle = conn,
                                     .attr_handle = (uint16_t)(0x10u + index),
                                     .offset = offset};
  return value->read(value->user, &access, out, capacity, out_len);
}
static void put16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8u);
}
static uint16_t u16(const uint8_t *data) {
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}
static uint64_t u64(const uint8_t *data) {
  uint64_t value = 0u;
  for (size_t i = 0u; i < 8u; ++i)
    value |= (uint64_t)data[i] << (8u * i);
  return value;
}
static h2_pal_result_t request_raw(fake_ble_t *ble, uint16_t conn,
                                   uint16_t offset, const uint8_t *data,
                                   size_t len) {
  const h2_pal_ble_gatt_characteristic_t *request =
      &ble->service->characteristics[1];
  h2_pal_ble_gatt_access_t access = {
      .conn_handle = conn, .attr_handle = 0x11u, .offset = offset};
  return request->write(request->user, &access, data, len);
}
static h2_pal_result_t request(fake_ble_t *ble, uint16_t conn,
                               uint64_t revision, uint16_t offset,
                               uint16_t limit) {
  uint8_t bytes[13] = {1u};
  for (size_t i = 0u; i < 8u; ++i)
    bytes[1u + i] = (uint8_t)(revision >> (i * 8u));
  put16(bytes + 9u, offset);
  put16(bytes + 11u, limit);
  return request_raw(ble, conn, 0u, bytes, sizeof(bytes));
}
static void connect_event(fake_ble_t *ble, uint16_t conn, uint16_t mtu) {
  h2_pal_ble_connection_t value = {
      .conn_handle = conn, .role = H2_PAL_BLE_ROLE_PERIPHERAL, .mtu = mtu};
  emit(ble, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &value, sizeof(value));
}
static void disconnect_event(fake_ble_t *ble, uint16_t conn) {
  h2_pal_ble_disconnected_info_t value = {.conn_handle = conn};
  emit(ble, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &value, sizeof(value));
}
static size_t decode_hex(const char *hex, uint8_t *out) {
  size_t len = strlen(hex);
  assert(len % 2u == 0u && len <= 488u);
  for (size_t i = 0u; i < len / 2u; ++i) {
    unsigned int byte;
    assert(sscanf(hex + 2u * i, "%2x", &byte) == 1);
    out[i] = (uint8_t)byte;
  }
  return len / 2u;
}
static void equals_hex(const uint8_t *actual, size_t len, const char *hex) {
  uint8_t expected[244];
  assert(len == decode_hex(hex, expected));
  assert(memcmp(actual, expected, len) == 0);
}
static const char *json_string(yyjson_val *value, const char *key) {
  const char *out = yyjson_get_str(yyjson_obj_get(value, key));
  assert(out != NULL);
  return out;
}

static void test_url_contract(yyjson_val *fixture) {
  yyjson_val *cases = yyjson_obj_get(fixture, "url_cases");
  size_t index, count;
  yyjson_val *value;
  yyjson_arr_foreach(cases, index, count, value) {
    char url[1025];
    size_t len = 99u;
    assert(h2_gizclaw_ble_binding_format_url(text(json_string(value, "origin")),
                                             text(json_string(value, "icon")),
                                             text(json_string(value, "name")),
                                             text(json_string(value, "secret")),
                                             url, sizeof(url),
                                             &len) == H2_PAL_OK);
    const char *expected = json_string(value, "url");
    assert(strcmp(url, expected) == 0 && len == strlen(expected));
    char exact[1025];
    assert(h2_gizclaw_ble_binding_format_url(text(json_string(value, "origin")),
                                             text(json_string(value, "icon")),
                                             text(json_string(value, "name")),
                                             text(json_string(value, "secret")),
                                             exact, len + 1u,
                                             &len) == H2_PAL_OK);
    memset(exact, 'x', sizeof(exact));
    assert(h2_gizclaw_ble_binding_format_url(text(json_string(value, "origin")),
                                             text(json_string(value, "icon")),
                                             text(json_string(value, "name")),
                                             text(json_string(value, "secret")),
                                             exact, strlen(expected),
                                             &len) == H2_PAL_ERR_TRUNCATED);
    assert(len == 0u);
    for (size_t i = 0u; i < strlen(expected); ++i)
      assert(exact[i] == 0);
  }
  const char *origins[] = {"http://ap.gizclaw.com",
                           "https://",
                           "https://ap.gizclaw.com/",
                           "https://u@ap.gizclaw.com",
                           "https://ap.gizclaw.com:0",
                           "https://ap.gizclaw.com:65536",
                           "https://ap.gizclaw.com:8443?q=1",
                           "https://-host.example",
                           "https://a..b",
                           "https://ap.gizclaw.com\n"};
  for (size_t i = 0u; i < sizeof(origins) / sizeof(origins[0]); ++i) {
    char out[200];
    size_t len;
    assert(h2_gizclaw_ble_binding_format_url(
               text(origins[i]), text(""), text(""),
               text("gizclaw_sk_v1_public_fixture_001"), out, sizeof(out),
               &len) == H2_PAL_ERR_INVALID_ARG);
    assert(out[0] == 0 && len == 0u);
  }
  const char *secrets[] = {"gizclaw_sk_v1_", "test-secret",
                           "gizclaw_sk_v1_foo/bar", "gizclaw_sk_v1_x?key=z",
                           "gizclaw_sk_v1_x y"};
  for (size_t i = 0u; i < sizeof(secrets) / sizeof(secrets[0]); ++i) {
    char out[200];
    size_t len;
    assert(h2_gizclaw_ble_binding_format_url(text("https://ap.gizclaw.com"),
                                             text(""), text(""),
                                             text(secrets[i]), out, sizeof(out),
                                             &len) == H2_PAL_ERR_INVALID_ARG);
  }
  const char *names[] = {"\xc0\x80",
                         "\xed\xa0\x80",
                         "\xf4\x90\x80\x80",
                         "\xe5\xae",
                         "line\nfeed",
                         "\xc2\x80",
                         "12345678901234567890123456789012345678901"};
  for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
    char out[200];
    size_t len;
    assert(h2_gizclaw_ble_binding_format_url(
               text("https://ap.gizclaw.com"), text(""), text(names[i]),
               text("gizclaw_sk_v1_public_fixture_001"), out, sizeof(out),
               &len) == H2_PAL_ERR_INVALID_ARG);
  }
  char out[200];
  memset(out, 'x', sizeof(out));
  assert(h2_gizclaw_ble_binding_format_url(
             text("https://ap.gizclaw.com"), text(""), text(""),
             text("gizclaw_sk_v1_public_fixture_001"), out, sizeof(out),
             NULL) == H2_PAL_ERR_INVALID_ARG);
  for (size_t i = 0u; i < sizeof(out); ++i)
    assert(out[i] == 0);
  yyjson_val *uuids = yyjson_obj_get(fixture, "uuid_att_hex");
  equals_hex(h2_gizclaw_ble_binding_service_uuid.data, 16u,
             json_string(uuids, "service"));
  equals_hex(h2_gizclaw_ble_binding_info_uuid.data, 16u,
             json_string(uuids, "info"));
  equals_hex(h2_gizclaw_ble_binding_request_uuid.data, 16u,
             json_string(uuids, "request"));
  equals_hex(h2_gizclaw_ble_binding_credential_uuid.data, 16u,
             json_string(uuids, "credential"));
}

static void test_discovery_and_transfer(yyjson_val *fixture) {
  test_env_t env;
  fake_ble_t ble;
  setup(&env, true);
  ble_setup(&ble);
  h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
  assert(!binding_snapshot(binding).open && !binding_snapshot(binding).ready);
  assert(ble.registrations == 0u && h2_atomic_load(&env.starts) == 0u);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  assert(ble.registrations == 1u && ble.starts == 1u);
  uint8_t bytes[514];
  size_t len;
  assert(read_value(&ble, 0u, 1u, 0u, bytes, sizeof(bytes), &len) == H2_PAL_OK);
  assert(len == 14u && bytes[1] == 0u && u16(bytes + 10u) == 0u);
  refresh(&env, false);
  yyjson_val *transfer = yyjson_obj_get(fixture, "transfer");
  assert(snapshot(&env).revision == 2u);
  assert(read_value(&ble, 0u, 1u, 0u, bytes, sizeof(bytes), &len) == H2_PAL_OK);
  equals_hex(bytes, len, json_string(transfer, "info_hex"));
  assert(!binding_snapshot(binding).connected &&
         binding_snapshot(binding).ready);
  uint8_t request_bytes[244];
  size_t request_len =
      decode_hex(json_string(transfer, "request_hex"), request_bytes);
  assert(request_raw(&ble, 1u, 0u, request_bytes, request_len) == H2_PAL_OK);
  assert(request_raw(&ble, 1u, 0u, request_bytes, request_len) == H2_PAL_OK);
  assert(request(&ble, 1u, 2u, 1u, 231u) == H2_PAL_ERR_BUSY);
  /* ESP-style full callback buffer, while the mobile client terminates at a
   * short ATT response. No automatic Read Blob / offset=0 re-callback occurs.
   */
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) == H2_PAL_OK);
  equals_hex(bytes, len, json_string(transfer, "first_response_hex"));
  assert(len < 23u - 1u);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
         H2_PAL_ERR_INVALID_STATE);
  const char *url = json_string(transfer, "url");
  char reconstructed[1025] = {0};
  size_t offset = 0u;
  for (; offset < strlen(url);) {
    assert(request(&ble, 1u, 2u, (uint16_t)offset, 231u) == H2_PAL_OK);
    assert(read_value(&ble, 2u, 1u, 0u, bytes, 22u, &len) == H2_PAL_OK);
    assert(bytes[0] == 1u && u64(bytes + 1u) == 2u);
    assert(u16(bytes + 9u) == offset && u16(bytes + 11u) == strlen(url));
    assert(len > 13u && len < 22u);
    memcpy(reconstructed + offset, bytes + 13u, len - 13u);
    offset += len - 13u;
  }
  assert(strcmp(reconstructed, url) == 0);
  assert(request(&ble, 1u, 2u, (uint16_t)strlen(url), 231u) == H2_PAL_OK);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) == H2_PAL_OK);
  equals_hex(bytes, len, json_string(transfer, "empty_response_hex"));
  assert(binding_snapshot(binding).pending_exposures == 1u);
  assert(h2_atomic_load(&env.starts) == 1u); /* BLE made no RPC. */
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_ERR_BUSY);
  assert(binding != NULL && !binding_snapshot(binding).open &&
         ble.service == NULL);
  h2_gizclaw_ble_binding_exposure_t exposure;
  assert(h2_gizclaw_ble_binding_next_exposure(binding, &exposure) == H2_PAL_OK);
  assert(exposure.revision == 2u && strcmp(exposure.key_name, "key-name") == 0);
  assert(h2_gizclaw_ble_binding_next_exposure(binding, &exposure) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(exposure.revision == 0u && exposure.key_name[0] == 0);
  assert(ble.other_service && ble.other_advertising);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK &&
         binding == NULL);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
  teardown(&env);
}

static void test_refresh_connection_and_boundaries(void) {
  test_env_t env;
  fake_ble_t ble;
  setup(&env, true);
  ble_setup(&ble);
  h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  refresh(&env, false);
  uint8_t bytes[514];
  size_t len = 99u;
  connect_event(&ble, 1u, 23u);
  connect_event(&ble, 2u, 247u);
  assert(
      !binding_snapshot(binding).connected); /* Other services may connect. */
  assert(read_value(&ble, 0u, 2u, 0u, bytes, sizeof(bytes), &len) ==
             H2_PAL_OK &&
         u16(bytes + 12u) == 231u);
  assert(request(&ble, 1u, 2u, 0u, 231u) == H2_PAL_OK);
  assert(request(&ble, 2u, 2u, 0u, 231u) == H2_PAL_ERR_BUSY);
  assert(read_value(&ble, 2u, 2u, 0u, bytes, sizeof(bytes), &len) ==
             H2_PAL_ERR_BUSY &&
         len == 0u);
  assert(read_value(&ble, 2u, 1u, 1u, bytes, sizeof(bytes), &len) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, 12u, &len) == H2_PAL_ERR_NO_SPACE);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, 13u, &len) == H2_PAL_ERR_NO_SPACE);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, 14u, &len) == H2_PAL_OK &&
         len == 14u);
  assert(binding_snapshot(binding).pending_exposures ==
         1u); /* Partial byte counts. */
  assert(request(&ble, 1u, 2u, 1025u, 231u) == H2_PAL_ERR_INVALID_ARG);
  assert(request(&ble, 1u, 2u, 0u, 0u) == H2_PAL_ERR_INVALID_ARG);
  assert(request(&ble, 1u, 2u, 0u, 232u) == H2_PAL_ERR_INVALID_ARG);
  assert(request(&ble, 1u, UINT64_C(0x0102030405060708), 0u, 1u) ==
         H2_PAL_ERR_INVALID_STATE);
  uint8_t malformed[14] = {1u};
  assert(request_raw(&ble, 1u, 0u, malformed, 12u) == H2_PAL_ERR_FORMAT);
  assert(request_raw(&ble, 1u, 0u, malformed, 14u) == H2_PAL_ERR_FORMAT);
  malformed[0] = 2u;
  assert(request_raw(&ble, 1u, 0u, malformed, 13u) == H2_PAL_ERR_FORMAT);
  assert(request_raw(&ble, 1u, 1u, malformed, 13u) == H2_PAL_ERR_INVALID_ARG);
  assert(read_value(&ble, 0u, 1u, 0u, bytes, 13u, &len) == H2_PAL_ERR_NO_SPACE);
  assert(request(&ble, 1u, 2u, 0u, 231u) == H2_PAL_OK);
  h2_atomic_store(&env.reply, false);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_OK);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
             H2_PAL_ERR_INVALID_STATE &&
         len == 0u);
  assert(read_value(&ble, 0u, 1u, 0u, bytes, sizeof(bytes), &len) ==
             H2_PAL_OK &&
         bytes[1] == H2_GIZCLAW_BLE_BINDING_BUSY && u16(bytes + 10u) == 0u);
  assert(!binding_snapshot(binding).ready);
  h2_atomic_store(&env.reply, true);
  finish(&env);
  uint64_t revision = snapshot(&env).revision;
  assert(revision != 2u &&
         request(&ble, 1u, 2u, 0u, 231u) == H2_PAL_ERR_INVALID_STATE);
  assert(request(&ble, 1u, revision, 0u, 231u) == H2_PAL_OK);
  disconnect_event(&ble, 2u); /* Unrelated disconnect does not clear request. */
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
             H2_PAL_OK &&
         u64(bytes + 1u) == revision);
  assert(request(&ble, 1u, revision, 0u, 231u) == H2_PAL_OK);
  disconnect_event(&ble, 1u);
  connect_event(&ble, 1u, 247u);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(request(&ble, 1u, revision, 0u, 231u) == H2_PAL_OK);
  h2_pal_ble_mtu_info_t mtu = {.conn_handle = 1u, .mtu = 64u};
  emit(&ble, H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED, &mtu, sizeof(mtu));
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
             H2_PAL_OK &&
         len == 62u);
  assert(request(&ble, 1u, revision, 0u, 231u) == H2_PAL_OK);
  connect_event(&ble, 1u,
                23u); /* Reused handle cannot inherit a pending request. */
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_api_key_state_close(env.state) == H2_PAL_OK);
  assert(request(&ble, 1u, revision, 0u, 231u) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  h2_gizclaw_ble_binding_exposure_t exposure;
  while (h2_gizclaw_ble_binding_next_exposure(binding, &exposure) ==
         H2_PAL_OK) {
  }
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
  teardown(&env);
}

static void test_stale_failed_and_timeout(void) {
  test_env_t env;
  fake_ble_t ble;
  setup(&env, true);
  ble_setup(&ble);
  h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  refresh(&env, false);
  env.revoke_result = H2_PAL_ERR_IO;
  refresh(&env, true);
  h2_gizclaw_api_key_snapshot_t key = snapshot(&env);
  assert(key.valid && key.stale && !key.busy);
  assert(request(&ble, 1u, key.revision, 0u, 1u) == H2_PAL_ERR_INVALID_STATE);
  env.revoke_result = H2_PAL_OK;
  env.create_result = H2_PAL_ERR_IO;
  refresh(&env, true);
  assert(!snapshot(&env).valid && !binding_snapshot(binding).ready);
  env.create_result = H2_PAL_OK;
  refresh(&env, false);
  h2_atomic_store(&env.reply, false);
  unsigned starts = h2_atomic_load(&env.starts);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_OK);
  wait_count(&env.starts, starts + 1u);
  h2_atomic_store(&env.now, 1100u);
  assert(!binding_snapshot(binding).ready &&
         binding_snapshot(binding).last_error == H2_PAL_ERR_TIMEOUT);
  uint8_t bytes[244];
  size_t len;
  assert(read_value(&ble, 0u, 1u, 0u, bytes, sizeof(bytes), &len) ==
             H2_PAL_OK &&
         bytes[1] == H2_GIZCLAW_BLE_BINDING_FAILED);
  assert(request(&ble, 1u, key.revision, 0u, 1u) == H2_PAL_ERR_INVALID_STATE);
  h2_atomic_store(&env.reply, true);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
  teardown(&env);
}

static void test_exposure_backpressure(void) {
  test_env_t env;
  fake_ble_t ble;
  setup(&env, true);
  ble_setup(&ble);
  h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  uint8_t bytes[244];
  size_t len;
  for (unsigned i = 0u; i < 9u; ++i) {
    assert(snprintf(env.key_name, sizeof(env.key_name), "key-%u", i) > 0);
    refresh(&env, false);
    uint64_t revision = snapshot(&env).revision;
    assert(request(&ble, 1u, revision, 0u, 1u) == H2_PAL_OK);
    assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
           (i < 8u ? H2_PAL_OK : H2_PAL_ERR_NO_SPACE));
    assert(len == (i < 8u ? 14u : 0u));
  }
  assert(binding_snapshot(binding).pending_exposures == 8u);
  h2_gizclaw_ble_binding_exposure_t exposure;
  assert(h2_gizclaw_ble_binding_next_exposure(binding, &exposure) ==
             H2_PAL_OK &&
         strcmp(exposure.key_name, "key-0") == 0);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
         H2_PAL_OK); /* Failed export retains request. */
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_ERR_BUSY);
  for (unsigned i = 1u; i <= 8u; ++i) {
    char expected[27];
    assert(snprintf(expected, sizeof(expected), "key-%u", i) > 0);
    assert(h2_gizclaw_ble_binding_next_exposure(binding, &exposure) ==
               H2_PAL_OK &&
           strcmp(exposure.key_name, expected) == 0);
  }
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
  teardown(&env);
}

static void test_retryable_cleanup_and_reopen(void) {
  test_env_t env;
  fake_ble_t ble;
  setup(&env, true);
  ble_setup(&ble);
  h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
  ble.fail_subscribe_at = 3u;
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_ERR_IO);
  assert(ble.subscriptions_created == ble.subscriptions_destroyed &&
         ble.service == NULL);
  ble.fail_subscribe_at = 0u;
  ble.register_result = H2_PAL_ERR_NO_SPACE;
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_ERR_NO_SPACE);
  assert(ble.service == NULL &&
         ble.subscriptions_created == ble.subscriptions_destroyed);
  ble.register_result = H2_PAL_OK;
  ble.create_result = H2_PAL_ERR_UNSUPPORTED;
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_ERR_UNSUPPORTED);
  assert(ble.service == NULL && ble.other_service && ble.other_advertising);
  ble.create_result = H2_PAL_OK;
  ble.start_result = H2_PAL_ERR_IO;
  ble.unregister_result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_ERR_IO);
  assert(ble.service != NULL && !ble.set.live &&
         !binding_snapshot(binding).open);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_ERR_IO &&
         binding != NULL);
  ble.unregister_result = H2_PAL_OK;
  ble.start_result = H2_PAL_OK;
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  ble.stop_result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_ERR_IO);
  assert(ble.set.live && ble.service != NULL &&
         !binding_snapshot(binding).open);
  ble.stop_result = H2_PAL_OK;
  ble.destroy_result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_ERR_IO);
  assert(ble.set.live && ble.service != NULL);
  ble.destroy_result = H2_PAL_OK;
  ble.unregister_result = H2_PAL_ERR_UNSUPPORTED;
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_ERR_UNSUPPORTED);
  assert(!ble.set.live && ble.service != NULL &&
         ble.other_service); /* Never unregister-all. */
  ble.unregister_result = H2_PAL_OK;
  ble.read_during_unregister = true;
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  struct h2_pal_ble_adv_set other_set = {.live = true};
  h2_pal_ble_adv_set_event_t other = {.set = &other_set,
                                      .status = H2_PAL_ERR_IO};
  emit(&ble, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, &other,
       sizeof(other));
  assert(binding_snapshot(binding).advertising);
  h2_pal_ble_adv_set_event_t own = {.set = &ble.set, .status = H2_PAL_OK};
  emit(&ble, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, &own,
       sizeof(own));
  unsigned starts = ble.starts;
  assert(h2_gizclaw_ble_binding_poll(binding) == H2_PAL_OK &&
         ble.starts == starts + 1u);
  emit(&ble, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED, NULL, 0u);
  /* Simulate PAL-owned retirement before Host-stop event completion. */
  ble.service = NULL;
  ble.set.live = false;
  assert(!binding_snapshot(binding).open &&
         !binding_snapshot(binding).advertising);
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
  assert(ble.other_service && ble.other_advertising);
  teardown(&env);
}

static void test_reopen_preserves_exposure_deduplication(void) {
  test_env_t env;
  fake_ble_t ble;
  setup(&env, true);
  ble_setup(&ble);
  refresh(&env, false);
  h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
  const uint64_t revision = snapshot(&env).revision;
  uint8_t bytes[244];
  size_t len;
  /* Keep the first exposure undrained across more windows than queue slots. */
  for (size_t i = 0u; i < H2_GIZCLAW_BLE_BINDING_EXPOSURE_MAX + 3u; ++i) {
    assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
    assert(request(&ble, 1u, revision, 0u, 1u) == H2_PAL_OK);
    assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
           H2_PAL_OK);
    assert(len == 14u);
    assert(binding_snapshot(binding).pending_exposures == 1u);
    assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  }
  h2_gizclaw_ble_binding_exposure_t exposure;
  assert(h2_gizclaw_ble_binding_next_exposure(binding, &exposure) == H2_PAL_OK);
  assert(exposure.revision == revision &&
         strcmp(exposure.key_name, "key-name") == 0);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  assert(request(&ble, 1u, revision, 0u, 1u) == H2_PAL_OK);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) == H2_PAL_OK);
  assert(binding_snapshot(binding).pending_exposures == 0u);
  /* A genuinely new revision still needs its own obligation record. */
  refresh(&env, false);
  const uint64_t next_revision = snapshot(&env).revision;
  assert(next_revision != revision);
  assert(request(&ble, 1u, next_revision, 0u, 1u) == H2_PAL_OK);
  assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) == H2_PAL_OK);
  assert(binding_snapshot(binding).pending_exposures == 1u);
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  assert(h2_gizclaw_ble_binding_next_exposure(binding, &exposure) == H2_PAL_OK);
  assert(exposure.revision == next_revision);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
  teardown(&env);
}

static void assert_info_state(fake_ble_t *ble, yyjson_val *states,
                              const char *name) {
  uint8_t bytes[244];
  size_t len;
  assert(read_value(ble, 0u, 1u, 0u, bytes, sizeof(bytes), &len) == H2_PAL_OK);
  yyjson_val *state = yyjson_obj_get(states, name);
  assert(state != NULL);
  equals_hex(bytes, len, json_string(state, "info_hex"));
}

static void test_quota_wire_and_recovery(yyjson_val *fixture) {
  yyjson_val *states = yyjson_obj_get(fixture, "info_states");
  for (unsigned old_key = 0u; old_key < 2u; ++old_key) {
    test_env_t env;
    fake_ble_t ble;
    setup(&env, true);
    ble_setup(&ble);
    if (old_key != 0u)
      refresh(&env, false);
    h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
    assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
    uint8_t bytes[244];
    size_t len;
    if (old_key != 0u) {
      assert(request(&ble, 1u, 2u, 0u, 1u) == H2_PAL_OK);
      assert(read_value(&ble, 2u, 1u, 0u, bytes, sizeof(bytes), &len) ==
             H2_PAL_OK);
    }
    env.create_rpc_error = true;
    env.create_rpc_code = H2_GIZCLAW_RPC_ERROR_RESOURCE_EXHAUSTED;
    refresh(&env, false);
    assert_info_state(&ble, states,
                      old_key != 0u ? "quota_retained_key" : "quota_no_key");
    h2_gizclaw_ble_binding_snapshot_t rejected = binding_snapshot(binding);
    assert(!rejected.ready && rejected.url_len == 0u &&
           rejected.info_flags == H2_GIZCLAW_BLE_BINDING_EXHAUSTED);
    assert(rejected.last_error == H2_GIZCLAW_API_KEY_ERR_EXHAUSTED &&
           rejected.has_rpc_error && rejected.rpc_error_code == 8);
    assert(snapshot(&env).valid == (old_key != 0u));
    assert(request(&ble, 1u, rejected.revision, 0u, 1u) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(binding_snapshot(binding).pending_exposures == old_key);
    for (unsigned i = 0u; i < h2_atomic_load(&env.starts); ++i)
      assert(env.methods[i] == 96); /* Never remove any key to evade quota. */
    env.create_rpc_error = false;
    h2_atomic_store(&env.reply, false);
    assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
           H2_PAL_OK);
    assert_info_state(&ble, states,
                      old_key != 0u ? "busy_retry_retained_key"
                                    : "busy_retry_no_key");
    h2_gizclaw_ble_binding_snapshot_t retry = binding_snapshot(binding);
    assert(retry.info_flags == H2_GIZCLAW_BLE_BINDING_BUSY && !retry.ready &&
           retry.last_error == H2_PAL_OK && !retry.has_rpc_error &&
           retry.rpc_error_code == 0);
    assert(request(&ble, 1u, retry.revision, 0u, 1u) ==
           H2_PAL_ERR_INVALID_STATE);
    h2_atomic_store(&env.reply, true);
    finish(&env);
    assert_info_state(&ble, states,
                      old_key != 0u ? "recovered_retained_key"
                                    : "recovered_no_key");
    assert(binding_snapshot(binding).ready &&
           !binding_snapshot(binding).has_rpc_error);
    assert(h2_gizclaw_api_key_state_close(env.state) == H2_PAL_OK);
    assert_info_state(&ble, states,
                      old_key != 0u ? "closed_retained_key" : "closed_no_key");
    assert(binding_snapshot(binding).info_flags ==
               H2_GIZCLAW_BLE_BINDING_CLOSED &&
           !binding_snapshot(binding).ready);
    assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
    h2_gizclaw_ble_binding_exposure_t exposure;
    if (old_key != 0u) {
      assert(h2_gizclaw_ble_binding_next_exposure(binding, &exposure) ==
             H2_PAL_OK);
      assert(exposure.revision == 2u &&
             strcmp(exposure.key_name, "key-name") == 0);
    }
    assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
    teardown(&env);
  }
  test_env_t env;
  fake_ble_t ble;
  setup(&env, true);
  ble_setup(&ble);
  h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  env.create_rpc_error = true;
  env.create_rpc_code = H2_GIZCLAW_RPC_ERROR_PERMISSION_DENIED;
  refresh(&env, false);
  assert_info_state(&ble, states, "failed_permission");
  h2_gizclaw_ble_binding_snapshot_t failed = binding_snapshot(binding);
  assert(failed.last_error == H2_GIZCLAW_ERR_REMOTE && failed.has_rpc_error &&
         failed.rpc_error_code == H2_GIZCLAW_RPC_ERROR_PERMISSION_DENIED);
  env.create_rpc_error = false;
  refresh(&env, false);
  env.create_result = H2_GIZCLAW_API_KEY_ERR_EXHAUSTED;
  env.create_rpc_error = true;
  env.create_rpc_code = H2_GIZCLAW_RPC_ERROR_RESOURCE_EXHAUSTED;
  refresh(&env, false);
  failed = binding_snapshot(binding);
  assert(!failed.ready && failed.info_flags == H2_GIZCLAW_BLE_BINDING_FAILED &&
         !failed.has_rpc_error && failed.rpc_error_code == 0);
  assert(!snapshot(&env).valid && snapshot(&env).key.secret[0] == 0);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
  teardown(&env);
}

typedef struct concurrent_reader {
  h2_pal_ble_gatt_characteristic_t info, request, credential;
  h2_atomic_bool_t finish;
  h2_atomic_uint_t iterations, closed_reads;
} concurrent_reader_t;

static void read_concurrently(void *user) {
  concurrent_reader_t *reader = user;
  const h2_pal_ble_gatt_access_t access = {.conn_handle = 1u};
  uint8_t bytes[244];
  const uint8_t request_bytes[13] = {1u, 2u, 0u, 0u, 0u, 0u, 0u,
                                     0u, 0u, 0u, 0u, 1u, 0u};
  while (!h2_atomic_load(&reader->finish)) {
    size_t len;
    int rc = reader->info.read(reader->info.user, &access, bytes, sizeof(bytes),
                               &len);
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_CLOSED);
    if (rc == H2_PAL_ERR_CLOSED) {
      assert(len == 0u);
      h2_atomic_fetch_add(&reader->closed_reads, 1u);
    } else {
      rc = reader->request.write(reader->request.user, &access, request_bytes,
                                 sizeof(request_bytes));
      assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_CLOSED);
      rc = reader->credential.read(reader->credential.user, &access, bytes,
                                   sizeof(bytes), &len);
      assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_CLOSED);
    }
    h2_atomic_fetch_add(&reader->iterations, 1u);
  }
}

static void test_stop_during_reads(void) {
  test_env_t env;
  fake_ble_t ble;
  setup(&env, true);
  ble_setup(&ble);
  refresh(&env, false);
  h2_gizclaw_ble_binding_t *binding = binding_open(&env, &ble);
  assert(h2_gizclaw_ble_binding_start(binding) == H2_PAL_OK);
  concurrent_reader_t reader = {.info = ble.service->characteristics[0],
                                .request = ble.service->characteristics[1],
                                .credential = ble.service->characteristics[2]};
  assert(h2_atomic_bool_init(&reader.finish, false) == H2_ATOMIC_OK);
  assert(h2_atomic_uint_init(&reader.iterations, 0u) == H2_ATOMIC_OK);
  assert(h2_atomic_uint_init(&reader.closed_reads, 0u) == H2_ATOMIC_OK);
  h2_pal_task_t *task = NULL;
  h2_pal_task_options_t options = {.name = "binding-read-test",
                                   .min_stack_size = 65536u};
  assert(h2_pal_task_start(h2_desktop_platform_task_api(), &options,
                           read_concurrently, &reader, &task) == H2_PAL_OK);
  wait_count(&reader.iterations, 100u);
  assert(h2_gizclaw_ble_binding_stop(binding) == H2_PAL_OK);
  wait_count(&reader.closed_reads, 10u);
  h2_atomic_store(&reader.finish, true);
  assert(h2_pal_task_join(h2_desktop_platform_task_api(), task) == H2_PAL_OK);
  h2_gizclaw_ble_binding_exposure_t exposure;
  assert(h2_gizclaw_ble_binding_next_exposure(binding, &exposure) == H2_PAL_OK);
  assert(exposure.revision == 2u && strcmp(exposure.key_name, "key-name") == 0);
  assert(binding_snapshot(binding).pending_exposures == 0u);
  assert(h2_gizclaw_ble_binding_close(&binding) == H2_PAL_OK);
  h2_atomic_bool_destroy(&reader.finish);
  h2_atomic_uint_destroy(&reader.iterations);
  h2_atomic_uint_destroy(&reader.closed_reads);
  teardown(&env);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  yyjson_doc *fixture = yyjson_read_file(argv[1], 0u, NULL, NULL);
  assert(fixture != NULL);
  yyjson_val *root = yyjson_doc_get_root(fixture);
  test_url_contract(root);
  test_discovery_and_transfer(root);
  test_refresh_connection_and_boundaries();
  test_stale_failed_and_timeout();
  test_exposure_backpressure();
  test_retryable_cleanup_and_reopen();
  test_reopen_preserves_exposure_deduplication();
  test_quota_wire_and_recovery(root);
  test_stop_during_reads();
  yyjson_doc_free(fixture);
  puts("gizclaw BLE binding protocol/lifecycle tests passed");
  return 0;
}
