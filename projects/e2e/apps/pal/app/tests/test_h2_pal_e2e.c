#include "h2_pal_e2e.h"

#include <assert.h>
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

int main(void) {
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
