#include "h2_pal_e2e.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_mqtt_client {
  int placeholder;
};

typedef struct fake_state {
  h2_pal_mqtt_client_config_t config;
  struct h2_pal_mqtt_client client;
  uint64_t now_ms;
  int open_calls;
  int connect_calls;
  int subscribe_calls;
  int publish_calls;
  int process_calls;
  int disconnect_calls;
  int close_calls;
  int fail_publish;
  int malformed_subscribe_ack;
  int reject_subscribe;
  int suppress_process_events;
} fake_state_t;

static h2_pal_result_t fake_time_now(void *user, uint64_t *out_ms) {
  fake_state_t *state = (fake_state_t *)user;
  *out_ms = state->now_ms;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_open(void *user,
                                 const h2_pal_mqtt_client_config_t *config,
                                 h2_pal_mqtt_client_t **out_client) {
  fake_state_t *state = (fake_state_t *)user;
  state->open_calls++;
  state->config = *config;
  *out_client = &state->client;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_connect(void *user,
                                    h2_pal_mqtt_client_t *client) {
  fake_state_t *state = (fake_state_t *)user;
  assert(client == &state->client);
  state->connect_calls++;
  const h2_pal_mqtt_event_t event = {.type = H2_PAL_MQTT_EVENT_CONNECTED};
  state->config.on_event(state->config.event_user, client, &event);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_disconnect(void *user,
                                       h2_pal_mqtt_client_t *client,
                                       uint32_t timeout_ms) {
  fake_state_t *state = (fake_state_t *)user;
  assert(client == &state->client);
  assert(timeout_ms == H2_PAL_E2E_MQTT_DEFAULT_TIMEOUT_MS);
  state->disconnect_calls++;
  const h2_pal_mqtt_event_t event = {
      .type = H2_PAL_MQTT_EVENT_DISCONNECTED,
  };
  state->config.on_event(state->config.event_user, client, &event);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_publish(void *user,
                                    h2_pal_mqtt_client_t *client,
                                    const h2_pal_mqtt_publish_t *message,
                                    uint16_t *out_packet_id) {
  (void)out_packet_id;
  fake_state_t *state = (fake_state_t *)user;
  assert(client == &state->client);
  assert(message->qos == H2_PAL_MQTT_QOS0);
  state->publish_calls++;
  return state->fail_publish ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t fake_subscribe(
    void *user,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_subscribe_request_t *request,
    uint16_t *out_packet_id) {
  (void)out_packet_id;
  fake_state_t *state = (fake_state_t *)user;
  assert(client == &state->client);
  assert(request->item_count == 1u);
  state->subscribe_calls++;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_unsubscribe(
    void *user,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_unsubscribe_request_t *request,
    uint16_t *out_packet_id) {
  (void)user;
  (void)client;
  (void)request;
  (void)out_packet_id;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t fake_process(void *user,
                                    h2_pal_mqtt_client_t *client,
                                    uint32_t timeout_ms) {
  fake_state_t *state = (fake_state_t *)user;
  assert(client == &state->client);
  assert(timeout_ms > 0u);
  state->process_calls++;
  if (state->suppress_process_events) {
    state->now_ms += timeout_ms;
    return H2_PAL_ERR_TIMEOUT;
  }
  if (state->process_calls == 1) {
    const h2_pal_mqtt_suback_result_t result =
        state->reject_subscribe ? H2_PAL_MQTT_SUBACK_FAILURE
                                : H2_PAL_MQTT_SUBACK_QOS0;
    const h2_pal_mqtt_event_t event = {
        .type = H2_PAL_MQTT_EVENT_SUBSCRIBE_ACK,
        .data.subscribe_ack = {
            .results = state->malformed_subscribe_ack ? NULL : &result,
            .result_count = 1u,
            .result = H2_PAL_OK,
        },
    };
    state->config.on_event(state->config.event_user, client, &event);
  } else {
    const h2_pal_mqtt_event_t event = {
        .type = H2_PAL_MQTT_EVENT_PUBLISH_RECEIVED,
        .data.publish_received = {
            .topic = state->config.client_id,
            .payload = {.data = (const uint8_t *)"payload", .len = 7u},
            .qos = H2_PAL_MQTT_QOS0,
        },
    };
    state->config.on_event(state->config.event_user, client, &event);
  }
  state->now_ms += timeout_ms;
  return H2_PAL_OK;
}

static void fake_close(void *user, h2_pal_mqtt_client_t *client) {
  fake_state_t *state = (fake_state_t *)user;
  assert(client == &state->client);
  state->close_calls++;
}

static h2_runtime_t make_runtime(fake_state_t *state) {
  static const h2_pal_mqtt_vtable_t mqtt_vtable = {
      .open = fake_open,
      .connect = fake_connect,
      .disconnect = fake_disconnect,
      .publish = fake_publish,
      .subscribe = fake_subscribe,
      .unsubscribe = fake_unsubscribe,
      .process = fake_process,
      .close = fake_close,
  };
  static const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = fake_time_now,
  };
  static h2_pal_mqtt_api_t mqtt;
  static h2_pal_time_api_t time;
  mqtt.user = state;
  mqtt.vtable = &mqtt_vtable;
  time.user = state;
  time.vtable = &time_vtable;
  h2_runtime_t runtime;
  memset(&runtime, 0, sizeof(runtime));
  runtime.mqtt = &mqtt;
  runtime.time = &time;
  return runtime;
}

static h2_pal_e2e_config_t make_config(uint8_t *network_buffer,
                                       size_t network_buffer_len) {
  const char *topic = "topic";
  const char *payload = "payload";
  h2_pal_e2e_config_t config;
  memset(&config, 0, sizeof(config));
  config.suite_mask = H2_PAL_E2E_SUITE_MQTT;
  config.mqtt.host = "broker";
  config.mqtt.port = 1883u;
  config.mqtt.transport = H2_PAL_MQTT_TRANSPORT_TCP;
  config.mqtt.client_id = (h2_pal_mqtt_str_t){topic, strlen(topic)};
  config.mqtt.topic = (h2_pal_mqtt_str_t){topic, strlen(topic)};
  config.mqtt.payload =
      (h2_pal_mqtt_bytes_t){(const uint8_t *)payload, strlen(payload)};
  config.mqtt.network_buffer = network_buffer;
  config.mqtt.network_buffer_len = network_buffer_len;
  return config;
}

static void test_success(void) {
  fake_state_t state;
  memset(&state, 0, sizeof(state));
  h2_runtime_t runtime = make_runtime(&state);
  uint8_t network_buffer[256];
  h2_pal_e2e_config_t config =
      make_config(network_buffer, sizeof(network_buffer));
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) == H2_PAL_OK);
  assert(result.complete == 1);
  assert(result.stage == H2_PAL_E2E_STAGE_COMPLETE);
  assert(result.selected == 1u && result.passed == 1u && result.failed == 0u);
  assert(result.connected_events == 1);
  assert(result.subscribe_ack_events == 1);
  assert(result.publish_echo_events == 1);
  assert(result.disconnected_events == 1);
  assert(state.open_calls == 1 && state.close_calls == 1);
}

static void test_publish_failure_still_closes(void) {
  fake_state_t state;
  memset(&state, 0, sizeof(state));
  state.fail_publish = 1;
  h2_runtime_t runtime = make_runtime(&state);
  uint8_t network_buffer[256];
  h2_pal_e2e_config_t config =
      make_config(network_buffer, sizeof(network_buffer));
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) == H2_PAL_ERR_IO);
  assert(result.complete == 1 && result.stage == H2_PAL_E2E_STAGE_PUBLISH);
  assert(result.failed == 1u && result.passed == 0u);
  assert(state.disconnect_calls == 0 && state.close_calls == 1);
}

static void test_subscribe_rejection_still_closes(void) {
  fake_state_t state;
  memset(&state, 0, sizeof(state));
  state.reject_subscribe = 1;
  h2_runtime_t runtime = make_runtime(&state);
  uint8_t network_buffer[256];
  h2_pal_e2e_config_t config =
      make_config(network_buffer, sizeof(network_buffer));
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) == H2_PAL_ERR_IO);
  assert(result.complete == 1 && result.stage == H2_PAL_E2E_STAGE_SUBSCRIBE);
  assert(result.subscribe_ack_events == 1 && result.failed == 1u);
  assert(state.publish_calls == 0 && state.close_calls == 1);
}

static void test_malformed_subscribe_ack_still_closes(void) {
  fake_state_t state;
  memset(&state, 0, sizeof(state));
  state.malformed_subscribe_ack = 1;
  h2_runtime_t runtime = make_runtime(&state);
  uint8_t network_buffer[256];
  h2_pal_e2e_config_t config =
      make_config(network_buffer, sizeof(network_buffer));
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(result.complete == 1 && result.stage == H2_PAL_E2E_STAGE_SUBSCRIBE);
  assert(result.subscribe_ack_events == 1 && result.failed == 1u);
  assert(state.publish_calls == 0 && state.close_calls == 1);
}

static void test_subscribe_timeout_still_closes(void) {
  fake_state_t state;
  memset(&state, 0, sizeof(state));
  state.suppress_process_events = 1;
  h2_runtime_t runtime = make_runtime(&state);
  uint8_t network_buffer[256];
  h2_pal_e2e_config_t config =
      make_config(network_buffer, sizeof(network_buffer));
  config.mqtt.timeout_ms = 500u;
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) == H2_PAL_ERR_TIMEOUT);
  assert(result.complete == 1 && result.stage == H2_PAL_E2E_STAGE_SUBSCRIBE);
  assert(result.subscribe_ack_events == 0 && result.failed == 1u);
  assert(state.process_calls == 2 && state.close_calls == 1);
}

static void test_invalid_config_reports_complete_result(void) {
  fake_state_t state;
  memset(&state, 0, sizeof(state));
  h2_runtime_t runtime = make_runtime(&state);
  h2_pal_e2e_config_t config;
  memset(&config, 0, sizeof(config));
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(result.complete == 1 && result.failed == 1u);
  assert(result.stage == H2_PAL_E2E_STAGE_PREFLIGHT);
  assert(state.open_calls == 0 && state.close_calls == 0);
}

static void test_preference_rejects_combined_suite_mask(void) {
  fake_state_t state;
  memset(&state, 0, sizeof(state));
  h2_runtime_t runtime = make_runtime(&state);
  uint8_t network_buffer[256];
  h2_pal_e2e_config_t config =
      make_config(network_buffer, sizeof(network_buffer));
  config.suite_mask |= H2_PAL_E2E_SUITE_PREF;
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(result.complete == 1 && result.failed == 1u);
  assert(result.stage == H2_PAL_E2E_STAGE_PREFLIGHT);
  assert(state.open_calls == 0 && state.close_calls == 0);
}

static void test_host_suite_records_every_failure(void) {
  h2_runtime_t runtime;
  memset(&runtime, 0, sizeof(runtime));
  h2_pal_e2e_config_t config;
  memset(&config, 0, sizeof(config));
  config.suite_mask = H2_PAL_E2E_SUITE_HOST;
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) != H2_PAL_OK);
  assert(result.complete == 1);
  assert(result.selected == 19u);
  assert(result.passed == 0u);
  assert(result.failed == result.selected);
  assert(result.case_count == result.selected);
}

typedef struct wifi_fixture {
  int disconnect_calls;
  int disconnect_result;
  int stale_ip;
  int omit_interface;
  uint32_t flags;
} wifi_fixture_t;

static int wifi_disconnect(void *user) {
  wifi_fixture_t *fixture = user;
  ++fixture->disconnect_calls;
  return fixture->disconnect_result;
}

static int wifi_status(void *user, h2_pal_wifi_sta_status_t *status) {
  wifi_fixture_t *fixture = user;
  assert(fixture->disconnect_calls != 0);
  memset(status, 0, sizeof(*status));
  status->state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  status->ip_valid = (uint8_t)(fixture->stale_ip != 0);
  return H2_PAL_OK;
}

static h2_pal_result_t wifi_netif_list(
    void *user, const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn callback, void *callback_user) {
  (void)filter;
  wifi_fixture_t *fixture = user;
  h2_pal_netif_status_t status = {0};
  if (fixture->omit_interface) return H2_PAL_OK;
  status.kind = H2_PAL_NETIF_KIND_WIFI_STA;
  status.ref.kind = status.kind;
  status.flags = fixture->flags;
  return callback(callback_user, &status.ref, &status);
}

static void test_wifi_disconnected_state_contract(void) {
  wifi_fixture_t fixture = {0};
  const h2_pal_wifi_sta_vtable_t sta_vtable = {
      .disconnect = wifi_disconnect, .get_status = wifi_status};
  const h2_pal_wifi_sta_api_t sta = { .user = &fixture, .vtable = &sta_vtable };
  const h2_pal_netif_vtable_t netif_vtable = {.list = wifi_netif_list};
  const h2_pal_netif_api_t netif = {.user = &fixture, .vtable = &netif_vtable};
  h2_runtime_t runtime = {0};
  runtime.wifi_sta = &sta;
  runtime.netif = &netif;
  h2_pal_e2e_config_t config = {.suite_mask = H2_PAL_E2E_SUITE_WIFI};
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) == H2_PAL_OK);
  assert(result.selected == 1 && result.passed == 1);
  const uint32_t bad_flags[] = {H2_PAL_NETIF_FLAG_LINK_UP,
      H2_PAL_NETIF_FLAG_HAS_IPV4, H2_PAL_NETIF_FLAG_DEFAULT_ROUTE};
  for (size_t i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); ++i) {
    fixture.flags = bad_flags[i];
    assert(h2_pal_e2e_run(&runtime, &config, &result) != H2_PAL_OK);
    assert(result.failed == 1);
  }
  fixture.flags = 0;
  fixture.stale_ip = 1;
  assert(h2_pal_e2e_run(&runtime, &config, &result) != H2_PAL_OK);
  fixture.stale_ip = 0;
  fixture.omit_interface = 1;
  assert(h2_pal_e2e_run(&runtime, &config, &result) != H2_PAL_OK);
  assert(result.cases[0].result == H2_PAL_ERR_NOT_FOUND);
  fixture.omit_interface = 0;
  fixture.disconnect_result = H2_PAL_ERR_IO;
  assert(h2_pal_e2e_run(&runtime, &config, &result) != H2_PAL_OK);
  assert(result.cases[0].result == H2_PAL_ERR_IO);
  const int calls = fixture.disconnect_calls;
  config.suite_mask |= H2_PAL_E2E_SUITE_MQTT;
  assert(h2_pal_e2e_run(&runtime, &config, &result) == H2_PAL_ERR_INVALID_ARG);
  assert(fixture.disconnect_calls == calls);
}

typedef struct queue_lifetime_fixture {
  int task_only;
  int log_calls;
  int log_in_case;
  h2_pal_task_entry_t entry;
  void *context;
  uint64_t now;
  int closed, joined, destroyed, allocations, allow_join;
} queue_lifetime_fixture_t;

static void *lifetime_alloc(void *user, size_t bytes) {
  queue_lifetime_fixture_t *f = user;
  ++f->allocations;
  return malloc(bytes);
}
static void lifetime_free(void *user, void *ptr) {
  queue_lifetime_fixture_t *f = user;
  assert(f->entry == NULL || f->joined);
  --f->allocations;
  free(ptr);
}
static h2_pal_result_t lifetime_now(void *user, uint64_t *now) {
  queue_lifetime_fixture_t *f = user;
  f->now += 1000u;
  *now = f->now;
  return H2_PAL_OK;
}
static h2_pal_result_t lifetime_sleep(void *user, uint32_t ms) {
  (void)user; (void)ms;
  return H2_PAL_OK;
}
static int lifetime_create(void *user, const h2_pal_queue_config_t *config,
                           h2_pal_queue_t **out) {
  (void)config;
  if (((queue_lifetime_fixture_t *)user)->task_only != 0)
    return H2_PAL_ERR_UNSUPPORTED;
  *out = (h2_pal_queue_t *)user;
  return H2_PAL_OK;
}
static int lifetime_send(void *user, h2_pal_queue_t *queue,
                         const void *item, uint32_t timeout) {
  (void)user; (void)queue; (void)item; (void)timeout;
  return H2_PAL_ERR_IO;
}
static int lifetime_recv(void *user, h2_pal_queue_t *queue,
                         void *item, uint32_t timeout) {
  queue_lifetime_fixture_t *f = user;
  (void)queue; (void)item; (void)timeout;
  assert(f->closed && !f->destroyed && f->allocations == 1);
  return H2_PAL_ERR_INVALID_STATE;
}
static int lifetime_close(void *user, h2_pal_queue_t *queue) {
  queue_lifetime_fixture_t *f = user;
  (void)queue;
  f->closed = 1;
  return H2_PAL_OK;
}
static void lifetime_destroy(void *user, h2_pal_queue_t *queue) {
  queue_lifetime_fixture_t *f = user;
  (void)queue;
  assert(f->joined);
  ++f->destroyed;
}
static int lifetime_start(void *user, const h2_pal_task_options_t *options,
                          h2_pal_task_entry_t entry, void *context,
                          h2_pal_task_t **out) {
  queue_lifetime_fixture_t *f = user;
  const char *name = f->task_only == 2 ? "pal/e2e/condition" :
      (f->task_only == 1 ? "pal/e2e/core" : "pal/e2e/queue");
  if (strcmp(options->name, name) != 0)
    return H2_PAL_ERR_UNSUPPORTED;
  f->entry = entry;
  f->context = context;
  *out = (h2_pal_task_t *)f;
  return H2_PAL_OK;
}
static int lifetime_join(void *user, h2_pal_task_t *task) {
  queue_lifetime_fixture_t *f = user;
  (void)task;
  assert((f->task_only || f->closed) && !f->destroyed);
  if (!f->allow_join) return H2_PAL_ERR_BUSY;
  f->entry(f->context);
  f->joined = 1;
  return H2_PAL_OK;
}
static h2_pal_result_t lifetime_mutex_create(void *user,
    const h2_pal_mutex_config_t *config, h2_pal_mutex_t **out) {
  if (strcmp(config->name, "pal-e2e-cond-mutex") != 0)
    return H2_PAL_ERR_UNSUPPORTED;
  *out = (h2_pal_mutex_t *)user;
  return H2_PAL_OK;
}
static h2_pal_result_t lifetime_mutex_use(void *user, h2_pal_mutex_t *mutex) {
  (void)mutex;
  assert(!((queue_lifetime_fixture_t *)user)->destroyed);
  return H2_PAL_OK;
}
static h2_pal_result_t lifetime_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
  (void)mutex;
  queue_lifetime_fixture_t *f = user;
  assert(f->joined);
  ++f->destroyed;
  return H2_PAL_OK;
}
static h2_pal_result_t lifetime_cond_create(void *user,
    const h2_pal_cond_config_t *config, h2_pal_cond_t **out) {
  (void)config;
  *out = (h2_pal_cond_t *)user;
  return H2_PAL_OK;
}
static h2_pal_result_t lifetime_cond_signal(void *user, h2_pal_cond_t *cond) {
  (void)user; (void)cond;
  return H2_PAL_OK;
}
static h2_pal_result_t lifetime_cond_destroy(void *user, h2_pal_cond_t *cond) {
  (void)cond;
  queue_lifetime_fixture_t *f = user;
  assert(f->joined);
  ++f->destroyed;
  return H2_PAL_OK;
}
static int lifetime_log(void *user, h2_pal_log_level_t level,
    const char *scope, const char *message) {
  queue_lifetime_fixture_t *f = user;
  assert(level == H2_PAL_LOG_INFO && strcmp(scope, "pal-e2e") == 0);
  if (strstr(message, "phase=begin") != NULL) {
    assert(!f->log_in_case);
    f->log_in_case = 1;
  } else {
    assert(strstr(message, "phase=end") != NULL && f->log_in_case);
    f->log_in_case = 0;
  }
  ++f->log_calls;
  /* Diagnostics failure must not abort the worker test or its cleanup. */
  return H2_PAL_ERR_IO;
}
static void test_join_failure_retains_context(int task_only) {
  queue_lifetime_fixture_t f = {.task_only=task_only};
  const h2_pal_mem_vtable_t mem_v = {.alloc=lifetime_alloc, .free=lifetime_free};
  const h2_pal_time_vtable_t time_v = {
      .get_monotonic_ms=lifetime_now, .sleep_ms=lifetime_sleep};
  const h2_pal_queue_vtable_t queue_v = {
      .create=lifetime_create, .send=lifetime_send, .recv=lifetime_recv,
      .close=lifetime_close, .destroy=lifetime_destroy};
  const h2_pal_task_vtable_t task_v = {.start=lifetime_start, .join=lifetime_join};
  const h2_pal_mem_api_t mem = {.user=&f, .vtable=&mem_v};
  const h2_pal_time_api_t time = {.user=&f, .vtable=&time_v};
  const h2_pal_queue_api_t queue = {.user=&f, .vtable=&queue_v};
  const h2_pal_task_api_t task = {.user=&f, .vtable=&task_v};
  const h2_pal_sync_vtable_t sync_v = {
      .create_mutex=lifetime_mutex_create, .destroy_mutex=lifetime_mutex_destroy,
      .lock_mutex=lifetime_mutex_use, .unlock_mutex=lifetime_mutex_use,
      .create_cond=lifetime_cond_create, .destroy_cond=lifetime_cond_destroy,
      .signal_cond=lifetime_cond_signal};
  const h2_pal_sync_api_t sync = {.user=&f, .vtable=&sync_v};
  const h2_pal_log_vtable_t log_v = {.write=lifetime_log};
  const h2_pal_log_api_t log = {.user=&f, .vtable=&log_v};
  h2_runtime_t runtime = {.mem=&mem, .time=&time, .queue=&queue, .task=&task,
      .sync=task_only == 2 ? &sync : NULL, .log=&log};
  fake_state_t mqtt_state = {0};
  const h2_runtime_t mqtt_runtime = make_runtime(&mqtt_state);
  runtime.mqtt = mqtt_runtime.mqtt;
  uint8_t network_buffer[256];
  h2_pal_e2e_config_t config = make_config(network_buffer, sizeof(network_buffer));
  config.suite_mask |= H2_PAL_E2E_SUITE_CORE;
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) != H2_PAL_OK);
  assert(result.retained_cleanup != NULL);
  assert(mqtt_state.open_calls == 0);
  assert(f.log_calls == (int)(2u * result.case_count) && !f.log_in_case);
  assert((task_only || f.closed) && !f.destroyed && f.allocations == 1);
  assert(h2_pal_e2e_cleanup(&runtime, &result) == H2_PAL_ERR_BUSY);
  assert(f.allocations == 1 && !f.destroyed);
  f.allow_join = 1;
  assert(h2_pal_e2e_cleanup(&runtime, &result) == H2_PAL_OK);
  assert(result.retained_cleanup == NULL && f.allocations == 0);
  assert(f.destroyed == (task_only == 2 ? 2 : (task_only ? 0 : 1)) && f.joined);
  assert(h2_pal_e2e_cleanup(&runtime, &result) == H2_PAL_OK);
  assert(f.allocations == 0);
}

struct h2_pal_fs_file { int placeholder; };
typedef struct fs_fixture {
  struct h2_pal_fs_file file;
  char bytes[64];
  size_t length;
  size_t position;
  int ignored_seek;
  int repeats_at_eof;
  int bad_type;
  int bad_size;
  int bad_directory;
  int accepts_file_as_directory;
  int stat_calls;
  int removes;
} fs_fixture_t;

static int fs_test_mkdir(void *user, const char *path) {
  if (strcmp(path, "/data/pal-host-e2e/value") == 0) {
    return ((fs_fixture_t *)user)->accepts_file_as_directory
        ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_OK;
}
static int fs_test_open(void *user, const char *path,
                        h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out) {
  fs_fixture_t *f = user;
  (void)path; (void)mode;
  f->position = 0u;
  *out = &f->file;
  return H2_PAL_OK;
}
static int fs_test_write(void *user, h2_pal_fs_file_t *file,
                         const void *data, size_t len, size_t *written) {
  fs_fixture_t *f = user;
  assert(file == &f->file && len <= sizeof(f->bytes));
  memcpy(f->bytes, data, len);
  f->length = len;
  *written = len;
  return H2_PAL_OK;
}
static int fs_test_read(void *user, h2_pal_fs_file_t *file,
                        void *data, size_t len, size_t *read_count) {
  fs_fixture_t *f = user;
  assert(file == &f->file && f->position <= f->length);
  if (f->repeats_at_eof && f->position == f->length) f->position = 0u;
  size_t count = f->length - f->position;
  if (count > len) count = len;
  memcpy(data, f->bytes + f->position, count);
  f->position += count;
  *read_count = count;
  return H2_PAL_OK;
}
static int fs_test_seek(void *user, h2_pal_fs_file_t *file, uint64_t position) {
  fs_fixture_t *f = user;
  assert(file == &f->file && position <= f->length);
  if (!f->ignored_seek) f->position = (size_t)position;
  return H2_PAL_OK;
}
static int fs_test_close(void *user, h2_pal_fs_file_t *file) {
  assert(file == &((fs_fixture_t *)user)->file);
  return H2_PAL_OK;
}
static int fs_test_stat(void *user, const char *path, h2_pal_fs_stat_t *out) {
  fs_fixture_t *f = user;
  if (strcmp(path, "/data/../escape") == 0) return H2_PAL_ERR_INVALID_ARG;
  ++f->stat_calls;
  if (strcmp(path, "/data/pal-host-e2e") == 0) {
    *out = (h2_pal_fs_stat_t){.is_dir=f->bad_directory ? 0 : 1};
    return H2_PAL_OK;
  }
  *out = (h2_pal_fs_stat_t){.size=f->length + (f->bad_size ? 1u : 0u),
                           .is_dir=f->bad_type ? 1 : 0};
  return H2_PAL_OK;
}
static int fs_test_remove(void *user, const char *path) {
  (void)path;
  ++((fs_fixture_t *)user)->removes;
  return H2_PAL_OK;
}
static void test_filesystem_stat_contract(void) {
  const h2_pal_fs_vtable_t v = {.mkdir=fs_test_mkdir, .open=fs_test_open,
      .write=fs_test_write, .read=fs_test_read, .close=fs_test_close,
      .stat=fs_test_stat, .remove=fs_test_remove, .seek=fs_test_seek};
  for (int scenario = 0; scenario < 7; ++scenario) {
    fs_fixture_t f = {0};
    f.bad_type = scenario == 1;
    f.bad_size = scenario == 2;
    f.bad_directory = scenario == 3;
    f.accepts_file_as_directory = scenario == 4;
    f.ignored_seek = scenario == 5;
    f.repeats_at_eof = scenario == 6;
    const h2_pal_fs_api_t fs = {.user=&f, .vtable=&v};
    h2_runtime_t runtime = {.fs=&fs};
    const h2_pal_e2e_config_t config = {.suite_mask=H2_PAL_E2E_SUITE_FILESYSTEM};
    h2_pal_e2e_result_t result;
    assert(h2_pal_e2e_run(&runtime, &config, &result) ==
           (scenario == 0 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE));
    assert(result.case_count == 1 && result.complete);
    assert(result.cases[0].case_id == H2_PAL_E2E_CASE_HOST_FILESYSTEM);
    assert(f.stat_calls == (scenario == 3 ? 1 : 2) && f.removes == 2);
  }
}

typedef struct timer_lifetime_fixture {
  h2_pal_timer_config_t config;
  int allow_destroy;
} timer_lifetime_fixture_t;
static h2_pal_result_t timer_test_create(void *user,
    const h2_pal_timer_config_t *config, h2_pal_timer_t **out) {
  timer_lifetime_fixture_t *f = user;
  f->config = *config;
  *out = (h2_pal_timer_t *)f;
  return H2_PAL_OK;
}
static h2_pal_result_t timer_test_destroy(void *user, h2_pal_timer_t *timer) {
  timer_lifetime_fixture_t *f = user;
  assert(timer == (h2_pal_timer_t *)f);
  return f->allow_destroy ? H2_PAL_OK : H2_PAL_ERR_BUSY;
}
static void test_timer_failed_destroy_retains_callback(void) {
  queue_lifetime_fixture_t memory = {0};
  timer_lifetime_fixture_t f = {0};
  const h2_pal_mem_vtable_t mem_v = {.alloc=lifetime_alloc, .free=lifetime_free};
  const h2_pal_mem_api_t mem = {.user=&memory, .vtable=&mem_v};
  const h2_pal_time_vtable_t time_v = {
      .get_monotonic_ms=lifetime_now, .sleep_ms=lifetime_sleep};
  const h2_pal_time_api_t time = {.user=&memory, .vtable=&time_v};
  const h2_pal_timer_vtable_t timer_v = {
      .create=timer_test_create, .destroy=timer_test_destroy};
  const h2_pal_timer_api_t timer = {.user=&f, .vtable=&timer_v};
  h2_runtime_t runtime = {.mem=&mem, .time=&time, .timer=&timer};
  const h2_pal_e2e_config_t config = {.suite_mask=H2_PAL_E2E_SUITE_CORE};
  h2_pal_e2e_result_t result;
  assert(h2_pal_e2e_run(&runtime, &config, &result) == H2_PAL_ERR_BUSY);
  assert(result.case_count == 2 && result.retained_cleanup != NULL);
  assert(memory.allocations == 1);
  f.config.cb(f.config.cb_user, (h2_pal_timer_t *)&f);
  assert(h2_pal_e2e_cleanup(&runtime, &result) == H2_PAL_ERR_BUSY);
  assert(memory.allocations == 1);
  f.allow_destroy = 1;
  assert(h2_pal_e2e_cleanup(&runtime, &result) == H2_PAL_OK);
  assert(memory.allocations == 0 && result.retained_cleanup == NULL);
  assert(h2_pal_e2e_cleanup(&runtime, &result) == H2_PAL_OK);
}

int main(void) {
  test_timer_failed_destroy_retains_callback();
  test_filesystem_stat_contract();
  test_join_failure_retains_context(0);
  test_join_failure_retains_context(1);
  test_join_failure_retains_context(2);
  test_wifi_disconnected_state_contract();
  test_success();
  test_publish_failure_still_closes();
  test_subscribe_rejection_still_closes();
  test_malformed_subscribe_ack_still_closes();
  test_subscribe_timeout_still_closes();
  test_invalid_config_reports_complete_result();
  test_preference_rejects_combined_suite_mask();
  test_host_suite_records_every_failure();
  return 0;
}
