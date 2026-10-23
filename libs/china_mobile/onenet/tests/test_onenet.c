#include "h2_onenet.h"
#include "h2_yyjson_json.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define PREFIX "$sys/product-1/device-1/"
#define MAX_INPUTS 16u

typedef struct input {
  char topic[256];
  char payload[2048];
} input_t;

typedef struct fixture {
  h2_pal_mem_api_t mem;
  h2_pal_mqtt_api_t mqtt;
  h2_pal_time_api_t time;
  h2_yyjson_json_t* json;
  const h2_pal_mqtt_client_config_t* mqtt_config;
  h2_onenet_t* client;
  size_t live_allocations;
  size_t alloc_calls;
  size_t fail_allocation;
  uint64_t now;
  h2_pal_result_t time_error;
  h2_pal_result_t open_error;
  h2_pal_result_t connect_error;
  h2_pal_result_t subscribe_error;
  h2_pal_result_t publish_error;
  h2_pal_result_t process_error;
  h2_pal_result_t disconnect_error;
  h2_pal_mqtt_suback_result_t suback;
  bool acknowledge;
  bool subscribing;
  bool in_mqtt_callback;
  bool reply_in_callback;
  bool inject_during_reply;
  bool disconnect_during_reply;
  bool inspect_types;
  unsigned opened;
  unsigned closed;
  unsigned publish_count;
  unsigned events[H2_ONENET_ERROR + 1u];
  h2_pal_result_t last_result;
  int32_t last_code;
  uint64_t command_handle;
  char last_id[H2_ONENET_REQUEST_ID_CAPACITY];
  char last_service[H2_ONENET_NAME_CAPACITY];
  char published_topic[256];
  char published_payload[2048];
  input_t inputs[MAX_INPUTS];
  size_t input_count;
} fixture_t;

static void* allocate(void* user, size_t size) {
  fixture_t* f = user;
  ++f->alloc_calls;
  if (f->fail_allocation != 0u && f->alloc_calls == f->fail_allocation)
    return NULL;
  void* result = malloc(size);
  if (result != NULL) ++f->live_allocations;
  return result;
}

static void* reallocate(void* user, void* pointer, size_t size) {
  fixture_t* f = user;
  if (pointer == NULL) return allocate(user, size);
  ++f->alloc_calls;
  if (f->fail_allocation != 0u && f->alloc_calls == f->fail_allocation)
    return NULL;
  assert(size != 0u);
  return realloc(pointer, size);
}

static void release(void* user, void* pointer) {
  fixture_t* f = user;
  if (pointer != NULL) {
    assert(f->live_allocations != 0u);
    --f->live_allocations;
    free(pointer);
  }
}

static const h2_pal_mem_vtable_t mem_vtable = {
    .alloc = allocate, .realloc = reallocate, .free = release};

static h2_pal_result_t clock_now(void* user, uint64_t* out) {
  fixture_t* f = user;
  *out = f->now;
  return f->time_error;
}

static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = clock_now};

static void mqtt_event(fixture_t* f, const h2_pal_mqtt_event_t* event) {
  f->in_mqtt_callback = true;
  f->mqtt_config->on_event(f->mqtt_config->event_user, (h2_pal_mqtt_client_t*)f,
                           event);
  f->in_mqtt_callback = false;
}

static h2_pal_result_t mqtt_open(void* user,
                                 const h2_pal_mqtt_client_config_t* config,
                                 h2_pal_mqtt_client_t** out) {
  fixture_t* f = user;
  *out = NULL;
  if (f->open_error != H2_PAL_OK) return f->open_error;
  assert(config->password.len == strlen("test-token"));
  assert(memcmp(config->password.data, "test-token", config->password.len) ==
         0);
  assert(config->clean_session == 1);
  assert(config->operation_timeout_ms == 100u);
  f->mqtt_config = config;
  *out = (h2_pal_mqtt_client_t*)f;
  ++f->opened;
  return H2_PAL_OK;
}

static h2_pal_result_t mqtt_connect(void* user, h2_pal_mqtt_client_t* client) {
  fixture_t* f = user;
  (void)client;
  return f->connect_error;
}

static h2_pal_result_t mqtt_disconnect(void* user, h2_pal_mqtt_client_t* client,
                                       uint32_t timeout_ms) {
  fixture_t* f = user;
  (void)client;
  assert(timeout_ms == 100u);
  return f->disconnect_error;
}

static void mqtt_close(void* user, h2_pal_mqtt_client_t* client) {
  fixture_t* f = user;
  (void)client;
  ++f->closed;
  f->mqtt_config = NULL;
  f->subscribing = false;
}

static h2_pal_result_t mqtt_subscribe(
    void* user, h2_pal_mqtt_client_t* client,
    const h2_pal_mqtt_subscribe_request_t* request, uint16_t* out_id) {
  fixture_t* f = user;
  (void)client;
  assert(request->item_count == 1u);
  assert(request->items[0].filter.len == strlen(PREFIX "thing/#"));
  assert(memcmp(request->items[0].filter.data, PREFIX "thing/#",
                request->items[0].filter.len) == 0);
  *out_id = 42u;
  f->subscribing = true;
  return f->subscribe_error;
}

static void deliver(fixture_t* f, const char* topic, const char* payload) {
  const h2_pal_mqtt_event_t event = {
      .type = H2_PAL_MQTT_EVENT_PUBLISH_RECEIVED,
      .data.publish_received =
          {
              .topic = {topic, strlen(topic)},
              .payload = {(const uint8_t*)payload, strlen(payload)},
          },
  };
  mqtt_event(f, &event);
}

static h2_pal_result_t mqtt_publish(void* user, h2_pal_mqtt_client_t* client,
                                    const h2_pal_mqtt_publish_t* message,
                                    uint16_t* out_id) {
  fixture_t* f = user;
  (void)client;
  assert(!f->in_mqtt_callback);
  assert(message->topic.len < sizeof(f->published_topic));
  assert(message->payload.len < sizeof(f->published_payload));
  memcpy(f->published_topic, message->topic.data, message->topic.len);
  f->published_topic[message->topic.len] = '\0';
  memcpy(f->published_payload, message->payload.data, message->payload.len);
  f->published_payload[message->payload.len] = '\0';
  ++f->publish_count;
  *out_id = 0u;
  if (f->inject_during_reply) {
    f->inject_during_reply = false;
    deliver(f, PREFIX "thing/property/set",
            "{\"id\":\"902\",\"params\":{\"enabled\":true}}");
  }
  if (f->disconnect_during_reply) {
    f->disconnect_during_reply = false;
    const h2_pal_mqtt_event_t event = {
        .type = H2_PAL_MQTT_EVENT_DISCONNECTED,
        .data.disconnected.result = H2_PAL_ERR_CLOSED,
    };
    mqtt_event(f, &event);
  }
  return f->publish_error;
}

static h2_pal_result_t mqtt_process(void* user, h2_pal_mqtt_client_t* client,
                                    uint32_t timeout_ms) {
  fixture_t* f = user;
  (void)client;
  (void)timeout_ms;
  if (f->subscribing && f->acknowledge) {
    f->subscribing = false;
    const h2_pal_mqtt_event_t ack = {
        .type = H2_PAL_MQTT_EVENT_SUBSCRIBE_ACK,
        .data.subscribe_ack = {.packet_id = 42u,
                               .results = &f->suback,
                               .result_count = 1u},
    };
    mqtt_event(f, &ack);
  }
  for (size_t i = 0u; i < f->input_count; ++i) {
    deliver(f, f->inputs[i].topic, f->inputs[i].payload);
    /* Provider input is borrowed only during its callback. */
    memset(f->inputs[i].payload, 'x', strlen(f->inputs[i].payload));
  }
  f->input_count = 0u;
  return f->process_error;
}

static const h2_pal_mqtt_vtable_t mqtt_vtable = {
    .open = mqtt_open,
    .connect = mqtt_connect,
    .disconnect = mqtt_disconnect,
    .publish = mqtt_publish,
    .subscribe = mqtt_subscribe,
    .process = mqtt_process,
    .close = mqtt_close,
};

static void on_event(void* user, h2_onenet_t* client,
                     const h2_onenet_event_t* event) {
  fixture_t* f = user;
  assert(!f->in_mqtt_callback);
  ++f->events[event->type];
  f->last_result = event->result;
  f->last_code = event->code;
  if (event->request_id != NULL) strcpy(f->last_id, event->request_id);
  if (event->service != NULL) strcpy(f->last_service, event->service);
  if (event->type != H2_ONENET_COMMAND) return;
  f->command_handle = event->command_handle;
  if (f->inspect_types) {
    h2_pal_json_value_t* value = NULL;
    const h2_pal_json_api_t* json = h2_yyjson_json_api(f->json);
    assert(h2_pal_json_object_get(json, event->data, "enabled", 7u, &value) ==
           H2_PAL_OK);
    bool enabled = false;
    assert(h2_pal_json_value_get_boolean(json, value, &enabled) == H2_PAL_OK);
    assert(enabled);
  }
  assert(h2_onenet_process(client, 0u) == H2_PAL_ERR_BUSY);
  assert(h2_onenet_connect(client, "test-token") == H2_PAL_ERR_BUSY);
  assert(h2_onenet_disconnect(client) == H2_PAL_ERR_BUSY);
  h2_onenet_t* retained = client;
  assert(h2_onenet_destroy(&retained) == H2_PAL_ERR_BUSY && retained == client);
  if (f->reply_in_callback)
    assert(h2_onenet_reply(client, event->command_handle, 200,
                           (const uint8_t*)"{\"result\":true}",
                           15u) == H2_PAL_OK);
}

static h2_onenet_config_t initialize(fixture_t* f) {
  memset(f, 0, sizeof(*f));
  f->mem = (h2_pal_mem_api_t){f, &mem_vtable};
  f->mqtt = (h2_pal_mqtt_api_t){f, &mqtt_vtable};
  f->time = (h2_pal_time_api_t){f, &time_vtable};
  f->acknowledge = true;
  f->suback = H2_PAL_MQTT_SUBACK_QOS1;
  assert(h2_yyjson_json_create(&f->mem, &f->json) == H2_PAL_OK);
  return (h2_onenet_config_t){
      .allocator = &f->mem,
      .mqtt = &f->mqtt,
      .json = h2_yyjson_json_api(f->json),
      .time = &f->time,
      .product_id = "product-1",
      .device_name = "device-1",
      .endpoint = {{"test.invalid", 12u}, 1883u},
      .transport = H2_PAL_MQTT_TRANSPORT_TCP,
      .limits = {1024u, 12u, 128u, 2u, 4u},
      .mqtt_buffer_bytes = 2048u,
      .keepalive_sec = 60u,
      .operation_timeout_ms = 100u,
      .response_timeout_ms = 500u,
      .initial_request_id = 1u,
      .on_event = on_event,
      .event_user = f,
  };
}

static void cleanup(fixture_t* f) {
  assert(h2_onenet_destroy(&f->client) == H2_PAL_OK);
  assert(h2_yyjson_json_destroy(&f->json) == H2_PAL_OK);
  assert(f->live_allocations == 0u);
  assert(f->opened == f->closed);
}

static void connect_ready(fixture_t* f, const h2_onenet_config_t* config) {
  assert(h2_onenet_create(config, &f->client) == H2_PAL_OK);
  assert(h2_onenet_connect(f->client, "test-token") == H2_PAL_OK);
  assert(!h2_onenet_is_connected(f->client));
  assert(h2_onenet_process(f->client, 0u) == H2_PAL_OK);
  assert(h2_onenet_is_connected(f->client));
  assert(f->events[H2_ONENET_CONNECTED] == 1u);
}

static void input(fixture_t* f, const char* topic, const char* payload) {
  assert(f->input_count < MAX_INPUTS);
  strcpy(f->inputs[f->input_count].topic, topic);
  strcpy(f->inputs[f->input_count].payload, payload);
  ++f->input_count;
}

static uint64_t post(fixture_t* f) {
  uint64_t id = 0u;
  const char params[] =
      "{\"battery\":{\"value\":42},\"enabled\":{\"value\":true}}";
  assert(h2_onenet_publish(f->client, H2_ONENET_PROPERTY_POST,
                           (const uint8_t*)params, strlen(params),
                           &id) == H2_PAL_OK);
  return id;
}

static void test_response_correlation(void) {
  fixture_t f;
  h2_onenet_config_t config = initialize(&f);
  connect_ready(&f, &config);
  assert(post(&f) == 1u);
  assert(strcmp(f.published_topic, PREFIX "thing/property/post") == 0);
  assert(strstr(f.published_payload, "\"value\":true") != NULL);
  input(&f, "$sys/other/device-1/thing/property/post/reply",
        "{\"id\":\"1\",\"code\":200}");
  input(&f, PREFIX "thing/event/post/reply", "{\"id\":\"1\",\"code\":200}");
  input(&f, PREFIX "thing/property/post/reply", "{\"id\":\"99\",\"code\":200}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(f.events[H2_ONENET_RESPONSE] == 0u);
  input(&f, PREFIX "thing/property/post/reply",
        "{\"id\":\"1\",\"code\":403,\"data\":{\"reason\":\"denied\"}}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(f.events[H2_ONENET_RESPONSE] == 1u);
  assert(f.last_code == 403 && f.last_result == H2_PAL_OK);
  input(&f, PREFIX "thing/property/post/reply", "{\"id\":\"1\",\"code\":200}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(f.events[H2_ONENET_RESPONSE] == 1u);
  assert(post(&f) == 2u);
  assert(post(&f) == 3u);
  uint64_t id = 99u;
  assert(h2_onenet_publish(f.client, H2_ONENET_EVENT_POST, (const uint8_t*)"{}",
                           2u, &id) == H2_PAL_ERR_FULL);
  assert(id == 0u);
  f.now = 500u;
  input(&f, PREFIX "thing/property/post/reply", "{\"id\":\"2\",\"code\":200}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(f.events[H2_ONENET_RESPONSE] == 3u);
  assert(f.last_result == H2_PAL_ERR_TIMEOUT);
  cleanup(&f);
}

static void test_commands_and_reconnect(void) {
  fixture_t f;
  h2_onenet_config_t config = initialize(&f);
  connect_ready(&f, &config);
  f.inspect_types = true;
  input(&f, PREFIX "thing/property/set",
        "{\"id\":\"0007\",\"version\":\"1.0\",\"params\":{\"enabled\":true}}");
  input(&f, PREFIX "thing/property/set",
        "{\"id\":\"0007\",\"params\":{\"enabled\":true}}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(f.events[H2_ONENET_COMMAND] == 1u);
  assert(strcmp(f.last_id, "0007") == 0);
  const uint64_t first_handle = f.command_handle;
  f.publish_error = -1000;
  assert(h2_onenet_reply(f.client, first_handle, 200, NULL, 0u) == -1000);
  f.publish_error = H2_PAL_OK;
  assert(h2_onenet_reply(f.client, first_handle, 200, NULL, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_onenet_reply(f.client, first_handle, 200, NULL, 0u) == H2_PAL_OK);
  assert(strcmp(f.published_topic, PREFIX "thing/property/set_reply") == 0);
  assert(strstr(f.published_payload, "\"id\":\"0007\"") != NULL);
  assert(h2_onenet_reply(f.client, first_handle, 200, NULL, 0u) ==
         H2_PAL_ERR_NOT_FOUND);
  f.inspect_types = false;
  f.reply_in_callback = true;
  input(&f, PREFIX "thing/property/get",
        "{\"id\":\"8\",\"params\":[\"enabled\"]}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(strcmp(f.published_topic, PREFIX "thing/property/get_reply") == 0);
  input(&f, PREFIX "thing/service/ring/invoke",
        "{\"id\":\"9\",\"params\":{\"seconds\":2}}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(strcmp(f.last_service, "ring") == 0);
  assert(strcmp(f.published_topic, PREFIX "thing/service/ring/invoke_reply") ==
         0);
  f.reply_in_callback = false;
  input(&f, PREFIX "thing/property/set", "{\"id\":\"10\",\"params\":{}}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  const uint64_t stale_handle = f.command_handle;
  assert(post(&f) == 1u);
  assert(h2_onenet_connect(f.client, "test-token") == H2_PAL_OK);
  assert(f.events[H2_ONENET_COMMAND_EXPIRED] == 1u);
  assert(f.events[H2_ONENET_RESPONSE] == 1u);
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(h2_onenet_reply(f.client, stale_handle, 200, NULL, 0u) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(post(&f) == 2u);
  cleanup(&f);
}

static void test_input_during_reply(void) {
  fixture_t f;
  h2_onenet_config_t config = initialize(&f);
  connect_ready(&f, &config);
  f.reply_in_callback = true;
  f.inject_during_reply = true;
  input(&f, PREFIX "thing/property/set", "{\"id\":\"901\",\"params\":{}}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(f.events[H2_ONENET_COMMAND] == 1u);
  f.now = 100u;
  assert(post(&f) == 1u);
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(f.events[H2_ONENET_COMMAND] == 2u);
  assert(strcmp(f.last_id, "902") == 0);
  assert(f.events[H2_ONENET_RESPONSE] == 0u);
  cleanup(&f);
}

static void test_terminal_error_during_reply(void) {
  for (unsigned failure = 0u; failure < 2u; ++failure) {
    fixture_t f;
    h2_onenet_config_t config = initialize(&f);
    config.limits.queued_messages = 1u;
    connect_ready(&f, &config);
    f.reply_in_callback = true;
    f.inject_during_reply = failure == 0u;
    f.disconnect_during_reply = failure == 1u;
    input(&f, PREFIX "thing/property/set", "{\"id\":\"901\",\"params\":{}}");
    const h2_pal_result_t expected =
        failure == 0u ? H2_PAL_ERR_NO_SPACE : H2_PAL_ERR_CLOSED;
    assert(h2_onenet_process(f.client, 0u) == expected);
    assert(!h2_onenet_is_connected(f.client));
    assert(f.events[H2_ONENET_COMMAND] == 1u);
    assert(f.events[H2_ONENET_ERROR] == 1u);
    assert(f.events[H2_ONENET_DISCONNECTED] == 1u);
    cleanup(&f);
  }
}

static void test_malformed_and_bounds(void) {
  fixture_t f;
  h2_onenet_config_t config = initialize(&f);
  connect_ready(&f, &config);
  const char* invalid[] = {"[]",
                           "{\"id\":1,\"params\":{}}",
                           "{\"id\":\"a\",\"params\":{}}",
                           "{\"id\":\"1\",\"id\":\"2\",\"params\":{}}",
                           "{\"id\":\"1\",\"params\":{},\"version\":\"2.0\"}",
                           "{\"id\":\"1\",\"params\":[]}",
                           "{\"id\":\"1\",\"params\":{}} trailing"};
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    input(&f, PREFIX "thing/property/set", invalid[i]);
    assert(h2_onenet_process(f.client, 0u) == H2_PAL_ERR_FORMAT);
  }
  assert(f.events[H2_ONENET_COMMAND] == 0u);
  uint64_t id = 10u;
  assert(h2_onenet_publish(f.client, H2_ONENET_PROPERTY_POST, NULL, 1u, &id) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(id == 0u);
  assert(h2_onenet_publish(f.client, H2_ONENET_DESIRED_GET,
                           (const uint8_t*)"[1]", 3u,
                           &id) == H2_PAL_ERR_FORMAT);
  assert(id == 0u);
  assert(h2_onenet_publish(f.client, H2_ONENET_DESIRED_GET,
                           (const uint8_t*)"[\"enabled\"]", 11u,
                           &id) == H2_PAL_OK);
  assert(strcmp(f.published_topic, PREFIX "thing/property/desired/get") == 0);
  for (size_t i = 0u; i < 5u; ++i)
    input(&f, PREFIX "thing/property/set", "{\"id\":\"1\",\"params\":{}}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_ERR_NO_SPACE);
  assert(!h2_onenet_is_connected(f.client));
  assert(f.events[H2_ONENET_RESPONSE] == 1u);
  cleanup(&f);
}

static void test_transport_failures(void) {
  for (unsigned stage = 0u; stage < 5u; ++stage) {
    fixture_t f;
    h2_onenet_config_t config = initialize(&f);
    assert(h2_onenet_create(&config, &f.client) == H2_PAL_OK);
    if (stage == 0u) f.open_error = -1000;
    if (stage == 1u) f.connect_error = -1000;
    if (stage == 2u) f.subscribe_error = -1000;
    if (stage <= 2u) {
      assert(h2_onenet_connect(f.client, "test-token") == -1000);
    } else {
      assert(h2_onenet_connect(f.client, "test-token") == H2_PAL_OK);
      if (stage == 3u) {
        f.acknowledge = false;
        f.now = 100u;
        assert(h2_onenet_process(f.client, 0u) == H2_PAL_ERR_TIMEOUT);
      } else {
        f.suback = H2_PAL_MQTT_SUBACK_FAILURE;
        assert(h2_onenet_process(f.client, 0u) == H2_PAL_ERR_UNAVAILABLE);
      }
    }
    cleanup(&f);
  }
  fixture_t f;
  h2_onenet_config_t config = initialize(&f);
  connect_ready(&f, &config);
  f.publish_error = H2_PAL_ERR_IO;
  uint64_t id = 99u;
  assert(h2_onenet_publish(f.client, H2_ONENET_EVENT_POST, (const uint8_t*)"{}",
                           2u, &id) == H2_PAL_ERR_IO);
  f.publish_error = H2_PAL_OK;
  assert(post(&f) == 2u);
  f.process_error = H2_PAL_ERR_WOULD_BLOCK;
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  f.process_error = -1000;
  assert(h2_onenet_process(f.client, 0u) == -1000);
  assert(f.last_result == -1000);
  cleanup(&f);
}

static void test_allocation_cleanup(void) {
  for (size_t failure = 1u; failure < 64u; ++failure) {
    fixture_t f;
    h2_onenet_config_t config = initialize(&f);
    f.fail_allocation = f.alloc_calls + failure;
    h2_onenet_t* client = (h2_onenet_t*)(uintptr_t)1u;
    const h2_pal_result_t rc = h2_onenet_create(&config, &client);
    if (rc == H2_PAL_OK) {
      f.client = client;
      cleanup(&f);
      return;
    }
    assert(rc == H2_PAL_ERR_NO_MEMORY);
    assert(client == NULL);
    f.fail_allocation = 0u;
    cleanup(&f);
  }
  assert(!"allocation failure sweep did not reach a complete create");
}

static void test_independent_clients(void) {
  fixture_t first;
  fixture_t second;
  h2_onenet_config_t one = initialize(&first);
  h2_onenet_config_t two = initialize(&second);
  two.initial_request_id = H2_ONENET_MAX_REQUEST_ID;
  connect_ready(&first, &one);
  connect_ready(&second, &two);
  assert(post(&first) == 1u);
  assert(post(&second) == H2_ONENET_MAX_REQUEST_ID);
  uint64_t id = 1u;
  assert(h2_onenet_publish(second.client, H2_ONENET_EVENT_POST,
                           (const uint8_t*)"{}", 2u, &id) == H2_PAL_ERR_FULL);
  first.disconnect_error = -1000;
  assert(h2_onenet_disconnect(first.client) == -1000);
  assert(h2_onenet_is_connected(second.client));
  cleanup(&first);
  cleanup(&second);
}

static void test_deadlines_and_json_allocation(void) {
  fixture_t f;
  h2_onenet_config_t config = initialize(&f);
  connect_ready(&f, &config);
  input(&f, PREFIX "thing/property/set", "{\"id\":\"1\",\"params\":{}}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  const uint64_t handle = f.command_handle;
  f.now = 500u;
  assert(h2_onenet_reply(f.client, handle, 200, NULL, 0u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_OK);
  assert(f.events[H2_ONENET_COMMAND_EXPIRED] == 1u);
  assert(h2_onenet_reply(f.client, handle, 200, NULL, 0u) ==
         H2_PAL_ERR_NOT_FOUND);
  const size_t allocations = f.live_allocations;
  f.fail_allocation = f.alloc_calls + 1u;
  uint64_t id = 99u;
  assert(h2_onenet_publish(f.client, H2_ONENET_PROPERTY_POST,
                           (const uint8_t*)"{}", 2u,
                           &id) == H2_PAL_ERR_NO_MEMORY);
  assert(id == 0u && f.live_allocations == allocations);
  f.fail_allocation = 0u;
  assert(post(&f) == 2u);
  char oversized[1100];
  memset(oversized, ' ', sizeof(oversized));
  oversized[0] = '{';
  oversized[sizeof(oversized) - 2u] = '}';
  oversized[sizeof(oversized) - 1u] = '\0';
  input(&f, PREFIX "thing/property/set", oversized);
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_ERR_NO_SPACE);
  cleanup(&f);

  config = initialize(&f);
  config.limits.json_depth = 3u;
  config.limits.json_values = 8u;
  connect_ready(&f, &config);
  const char deep[] = "{\"a\":{\"b\":{\"c\":1}}}";
  assert(h2_onenet_publish(f.client, H2_ONENET_PROPERTY_POST,
                           (const uint8_t*)deep, strlen(deep),
                           &id) == H2_PAL_ERR_NO_SPACE);
  input(&f, PREFIX "thing/property/set",
        "{\"id\":\"1\",\"params\":{\"a\":[1,2,3,4,5,6]}}");
  assert(h2_onenet_process(f.client, 0u) == H2_PAL_ERR_NO_SPACE);
  assert(f.events[H2_ONENET_COMMAND] == 0u);
  cleanup(&f);
}

static void test_invalid_configuration(void) {
  fixture_t f;
  h2_onenet_config_t config = initialize(&f);
  h2_onenet_t* client = (h2_onenet_t*)(uintptr_t)1u;
  config.limits.pending_requests = SIZE_MAX;
  assert(h2_onenet_create(&config, &client) == H2_PAL_ERR_INVALID_ARG);
  assert(client == NULL);
  config.limits.pending_requests = 1u;
  config.limits.queued_messages = SIZE_MAX;
  assert(h2_onenet_create(&config, &client) == H2_PAL_ERR_INVALID_ARG);
  config.limits.queued_messages = 1u;
  config.endpoint.host = (h2_pal_mqtt_str_t){"a\0b", 3u};
  assert(h2_onenet_create(&config, &client) == H2_PAL_ERR_INVALID_ARG);
  config.endpoint.host = (h2_pal_mqtt_str_t){"test.invalid", 12u};
  config.transport = H2_PAL_MQTT_TRANSPORT_TLS;
  assert(h2_onenet_create(&config, &client) == H2_PAL_ERR_INVALID_ARG);
  assert(f.opened == 0u);
  cleanup(&f);
}

static void test_observed_disconnect(void) {
  fixture_t f;
  h2_onenet_config_t config = initialize(&f);
  connect_ready(&f, &config);
  assert(post(&f) == 1u);
  const h2_pal_mqtt_event_t closed = {
      .type = H2_PAL_MQTT_EVENT_DISCONNECTED,
      .data.disconnected = {.result = H2_PAL_ERR_CLOSED},
  };
  mqtt_event(&f, &closed);
  assert(!h2_onenet_is_connected(f.client));
  f.disconnect_error = H2_PAL_ERR_INVALID_STATE;
  assert(h2_onenet_disconnect(f.client) == H2_PAL_OK);
  assert(f.events[H2_ONENET_RESPONSE] == 1u);
  assert(f.last_result == H2_PAL_ERR_CLOSED);
  cleanup(&f);
}

int main(void) {
  test_response_correlation();
  test_commands_and_reconnect();
  test_input_during_reply();
  test_terminal_error_during_reply();
  test_malformed_and_bounds();
  test_transport_failures();
  test_allocation_cleanup();
  test_independent_clients();
  test_deadlines_and_json_allocation();
  test_invalid_configuration();
  test_observed_disconnect();
  return 0;
}
