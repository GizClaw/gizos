#include "h2_onenet.h"
#include "h2_onenet_internal.h"

#include <limits.h>
#include <string.h>

#define TOPIC_CAPACITY 256u

typedef struct pending_request {
  bool used;
  h2_onenet_operation_t operation;
  char id[H2_ONENET_REQUEST_ID_CAPACITY];
  char service[H2_ONENET_NAME_CAPACITY];
  uint64_t handle;
  uint64_t started_ms;
} pending_request_t;

typedef struct received_message {
  h2_onenet_operation_t operation;
  bool response;
  char service[H2_ONENET_NAME_CAPACITY];
  size_t length;
  uint64_t received_ms;
} received_message_t;

struct h2_onenet {
  h2_onenet_config_t config;
  h2_pal_mqtt_client_t* mqtt_client;
  h2_pal_mqtt_client_config_t mqtt_config;
  pending_request_t* outbound;
  pending_request_t* inbound;
  received_message_t* messages;
  uint8_t* received_data;
  uint8_t* mqtt_buffer;
  char* output;
  char host[H2_ONENET_HOST_CAPACITY];
  char product[H2_ONENET_NAME_CAPACITY];
  char device[H2_ONENET_NAME_CAPACITY];
  char client_id[H2_ONENET_CLIENT_ID_CAPACITY];
  char token[H2_ONENET_TOKEN_CAPACITY];
  char prefix[2u * H2_ONENET_NAME_CAPACITY + 8u];
  size_t prefix_len;
  size_t received_count;
  uint64_t next_id;
  uint64_t next_handle;
  uint64_t subscribe_started_ms;
  uint16_t subscribe_id;
  h2_pal_result_t receive_error;
  h2_pal_result_t transport_error;
  bool subscribing;
  bool ready;
  bool connected_event;
  bool busy;
  bool dispatching;
};

typedef struct operation_topic {
  h2_onenet_operation_t operation;
  const char* request;
  const char* response;
} operation_topic_t;

static const operation_topic_t topics[] = {
    {H2_ONENET_PROPERTY_POST, "thing/property/post",
     "thing/property/post/reply"},
    {H2_ONENET_EVENT_POST, "thing/event/post", "thing/event/post/reply"},
    {H2_ONENET_DESIRED_GET, "thing/property/desired/get",
     "thing/property/desired/get/reply"},
    {H2_ONENET_DESIRED_DELETE, "thing/property/desired/delete",
     "thing/property/desired/delete/reply"},
    {H2_ONENET_PROPERTY_SET, "thing/property/set", "thing/property/set_reply"},
    {H2_ONENET_PROPERTY_GET, "thing/property/get", "thing/property/get_reply"},
};

static bool deadline_expired(uint64_t started_ms, uint64_t now_ms,
                             uint32_t timeout_ms) {
  /* A queued message may predate a request created after the last pump.
   * Wrap-safe deadline comparison must not treat that older time as elapsed. */
  return h2_pal_time_deadline_expired(
      now_ms, h2_pal_time_deadline_ms(started_ms, timeout_ms));
}

static bool copy_text(char* out, size_t capacity, const char* text) {
  if (text == NULL) return false;
  size_t len = 0u;
  while (len < capacity && text[len] != '\0') ++len;
  if (len == capacity) return false;
  memcpy(out, text, len + 1u);
  return true;
}

static void emit(h2_onenet_t* client, const h2_onenet_event_t* event) {
  client->dispatching = true;
  client->config.on_event(client->config.event_user, client, event);
  client->dispatching = false;
}

static void emit_result(h2_onenet_t* client, h2_onenet_event_type_t type,
                        const pending_request_t* request,
                        h2_pal_result_t result) {
  const h2_onenet_event_t event = {
      .type = type,
      .operation = request != NULL ? request->operation : 0,
      .result = result,
      .request_id = request != NULL ? request->id : NULL,
      .service = request != NULL ? request->service : NULL,
      .command_handle = request != NULL ? request->handle : 0u,
  };
  emit(client, &event);
}

static pending_request_t* empty_slot(pending_request_t* requests,
                                     size_t count) {
  for (size_t i = 0u; i < count; ++i)
    if (!requests[i].used) return &requests[i];
  return NULL;
}

static void cancel_requests(h2_onenet_t* client, h2_pal_result_t result,
                            bool notify) {
  for (size_t i = 0u; i < client->config.limits.pending_requests; ++i) {
    if (client->outbound[i].used) {
      client->outbound[i].used = false;
      if (notify)
        emit_result(client, H2_ONENET_RESPONSE, &client->outbound[i], result);
    }
    if (client->inbound[i].used) {
      client->inbound[i].used = false;
      if (notify)
        emit_result(client, H2_ONENET_COMMAND_EXPIRED, &client->inbound[i],
                    result);
    }
  }
}

static void close_transport(h2_onenet_t* client, h2_pal_result_t reason,
                            bool notify) {
  const bool had_transport = client->mqtt_client != NULL;
  client->ready = false;
  client->subscribing = false;
  client->connected_event = false;
  if (client->mqtt_client != NULL) {
    h2_pal_mqtt_close(client->config.mqtt, client->mqtt_client);
    client->mqtt_client = NULL;
  }
  if (client->mqtt_buffer != NULL)
    h2_onenet_wipe(client->mqtt_buffer, client->config.mqtt_buffer_bytes);
  client->received_count = 0u;
  cancel_requests(client, reason, notify);
  h2_onenet_wipe(client->token, sizeof(client->token));
  if (notify && had_transport)
    emit_result(client, H2_ONENET_DISCONNECTED, NULL, reason);
}

static bool topic_equal(const char* data, size_t length, const char* expected) {
  return length == strlen(expected) && memcmp(data, expected, length) == 0;
}

static bool classify_topic(const h2_onenet_t* client, const char* topic,
                           size_t length, received_message_t* message) {
  if (topic == NULL || length < client->prefix_len ||
      memcmp(topic, client->prefix, client->prefix_len) != 0)
    return false;
  const char* suffix = topic + client->prefix_len;
  const size_t suffix_len = length - client->prefix_len;
  for (size_t i = 0u; i < sizeof(topics) / sizeof(topics[0]); ++i) {
    const bool outbound = topics[i].operation <= H2_ONENET_DESIRED_DELETE;
    if (topic_equal(suffix, suffix_len,
                    outbound ? topics[i].response : topics[i].request)) {
      message->operation = topics[i].operation;
      message->response = outbound;
      return true;
    }
  }
  const char service_prefix[] = "thing/service/";
  const char service_suffix[] = "/invoke";
  const size_t begin = sizeof(service_prefix) - 1u;
  const size_t end = sizeof(service_suffix) - 1u;
  if (suffix_len <= begin + end || memcmp(suffix, service_prefix, begin) != 0 ||
      memcmp(suffix + suffix_len - end, service_suffix, end) != 0)
    return false;
  const size_t service_len = suffix_len - begin - end;
  if (service_len >= sizeof(message->service)) return false;
  memcpy(message->service, suffix + begin, service_len);
  message->service[service_len] = '\0';
  if (!h2_onenet_segment_valid(message->service)) return false;
  message->operation = H2_ONENET_SERVICE_INVOKE;
  return true;
}

static void on_mqtt_event(void* user, h2_pal_mqtt_client_t* mqtt_client,
                          const h2_pal_mqtt_event_t* event) {
  h2_onenet_t* client = user;
  (void)mqtt_client;
  if (event->type == H2_PAL_MQTT_EVENT_SUBSCRIBE_ACK && client->subscribing &&
      event->data.subscribe_ack.packet_id == client->subscribe_id) {
    const h2_pal_mqtt_subscribe_ack_event_t* ack = &event->data.subscribe_ack;
    if (ack->result != H2_PAL_OK || ack->result_count != 1u ||
        ack->results == NULL ||
        (ack->results[0] != H2_PAL_MQTT_SUBACK_QOS0 &&
         ack->results[0] != H2_PAL_MQTT_SUBACK_QOS1)) {
      client->transport_error =
          ack->result != H2_PAL_OK ? ack->result : H2_PAL_ERR_UNAVAILABLE;
    } else {
      client->ready = true;
      client->connected_event = true;
      client->subscribing = false;
    }
  } else if (event->type == H2_PAL_MQTT_EVENT_DISCONNECTED) {
    client->ready = false;
    client->transport_error = event->data.disconnected.result != H2_PAL_OK
                                  ? event->data.disconnected.result
                                  : H2_PAL_ERR_CLOSED;
  } else if (event->type == H2_PAL_MQTT_EVENT_ERROR) {
    client->ready = false;
    client->transport_error = event->data.error.result != H2_PAL_OK
                                  ? event->data.error.result
                                  : H2_PAL_ERR_IO;
  } else if (event->type == H2_PAL_MQTT_EVENT_PUBLISH_RECEIVED) {
    const h2_pal_mqtt_publish_received_event_t* incoming =
        &event->data.publish_received;
    received_message_t message = {0};
    if (!classify_topic(client, incoming->topic.data, incoming->topic.len,
                        &message))
      return;
    if (client->received_count == client->config.limits.queued_messages ||
        incoming->payload.len > client->config.limits.payload_bytes ||
        (incoming->payload.data == NULL && incoming->payload.len != 0u)) {
      client->receive_error = H2_PAL_ERR_NO_SPACE;
      return;
    }
    h2_pal_result_t rc =
        h2_pal_time_get_monotonic_ms(client->config.time, &message.received_ms);
    if (rc != H2_PAL_OK) {
      client->receive_error = rc;
      return;
    }
    message.length = incoming->payload.len;
    const size_t index = client->received_count++;
    client->messages[index] = message;
    if (message.length != 0u)
      memcpy(
          client->received_data + index * client->config.limits.payload_bytes,
          incoming->payload.data, message.length);
  }
}

static bool json_api_complete(const h2_pal_json_api_t* json) {
  if (json == NULL || json->vtable == NULL) return false;
  const h2_pal_json_vtable_t* v = json->vtable;
  return v->document_parse != NULL && v->document_destroy != NULL &&
         v->document_root != NULL && v->value_type != NULL &&
         v->value_get_string != NULL && v->value_get_number != NULL &&
         v->object_get != NULL && v->array_size != NULL &&
         v->array_get != NULL && v->document_serialize != NULL &&
         v->buffer_release != NULL;
}

static bool config_valid(const h2_onenet_config_t* config) {
  if (config == NULL || config->allocator == NULL ||
      config->allocator->vtable == NULL ||
      config->allocator->vtable->alloc == NULL ||
      config->allocator->vtable->free == NULL || config->mqtt == NULL ||
      config->mqtt->vtable == NULL || config->time == NULL ||
      config->time->vtable == NULL ||
      config->time->vtable->get_monotonic_ms == NULL ||
      !json_api_complete(config->json) || config->on_event == NULL ||
      !h2_onenet_segment_valid(config->product_id) ||
      !h2_onenet_segment_valid(config->device_name) ||
      config->endpoint.host.data == NULL || config->endpoint.host.len == 0u ||
      config->endpoint.host.len >= H2_ONENET_HOST_CAPACITY ||
      memchr(config->endpoint.host.data, '\0', config->endpoint.host.len) !=
          NULL ||
      config->endpoint.port == 0u || config->keepalive_sec == 0u ||
      config->operation_timeout_ms == 0u || config->response_timeout_ms == 0u ||
      config->mqtt_buffer_bytes == 0u ||
      config->limits.payload_bytes < H2_ONENET_MIN_PAYLOAD_BYTES ||
      config->limits.payload_bytes == SIZE_MAX ||
      config->limits.json_depth < H2_ONENET_MIN_JSON_DEPTH ||
      config->limits.json_values < H2_ONENET_MIN_JSON_VALUES ||
      config->limits.pending_requests == 0u ||
      config->limits.pending_requests > SIZE_MAX / sizeof(pending_request_t) ||
      config->limits.queued_messages == 0u ||
      config->limits.queued_messages > SIZE_MAX / sizeof(received_message_t) ||
      config->limits.queued_messages >
          SIZE_MAX / config->limits.payload_bytes ||
      config->initial_request_id == 0u ||
      config->initial_request_id > H2_ONENET_MAX_REQUEST_ID)
    return false;
  if (config->transport != H2_PAL_MQTT_TRANSPORT_TCP &&
      config->transport != H2_PAL_MQTT_TRANSPORT_TLS)
    return false;
  if (config->transport == H2_PAL_MQTT_TRANSPORT_TLS && config->tls == NULL)
    return false;
  const h2_pal_mqtt_vtable_t* v = config->mqtt->vtable;
  return v->open != NULL && v->connect != NULL && v->close != NULL &&
         v->subscribe != NULL && v->publish != NULL && v->process != NULL &&
         v->disconnect != NULL;
}

h2_pal_result_t h2_onenet_create(const h2_onenet_config_t* config,
                                 h2_onenet_t** out_client) {
  if (out_client == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_client = NULL;
  if (!config_valid(config)) return H2_PAL_ERR_INVALID_ARG;
  h2_onenet_t* client = h2_pal_mem_alloc(config->allocator, sizeof(*client));
  if (client == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(client, 0, sizeof(*client));
  client->config = *config;
  client->next_id = config->initial_request_id;
  client->next_handle = 1u;
  memcpy(client->host, config->endpoint.host.data, config->endpoint.host.len);
  (void)copy_text(client->product, sizeof(client->product), config->product_id);
  (void)copy_text(client->device, sizeof(client->device), config->device_name);
  h2_pal_result_t rc =
      h2_onenet_build_client_id(client->device, config->identity,
                                client->client_id, sizeof(client->client_id));
  if (rc != H2_PAL_OK) goto failed;
  /* The copied strings below, not the caller's temporary config, are retained.
   */
  client->config.product_id = client->product;
  client->config.device_name = client->device;
  client->config.identity = NULL;
  client->config.endpoint.host.data = client->host;
  h2_onenet_writer_t prefix = {client->prefix, sizeof(client->prefix), 0u,
                               H2_PAL_OK};
  h2_onenet_text(&prefix, "$sys/");
  h2_onenet_text(&prefix, client->product);
  h2_onenet_text(&prefix, "/");
  h2_onenet_text(&prefix, client->device);
  h2_onenet_text(&prefix, "/");
  client->prefix_len = prefix.length;
  rc = prefix.result;
  if (rc != H2_PAL_OK) goto failed;
  const size_t pending_bytes =
      config->limits.pending_requests * sizeof(pending_request_t);
  client->outbound = h2_pal_mem_alloc(config->allocator, pending_bytes);
  client->inbound = h2_pal_mem_alloc(config->allocator, pending_bytes);
  client->messages =
      h2_pal_mem_alloc(config->allocator, config->limits.queued_messages *
                                              sizeof(*client->messages));
  client->received_data =
      h2_pal_mem_alloc(config->allocator, config->limits.queued_messages *
                                              config->limits.payload_bytes);
  client->mqtt_buffer =
      h2_pal_mem_alloc(config->allocator, config->mqtt_buffer_bytes);
  client->output =
      h2_pal_mem_alloc(config->allocator, config->limits.payload_bytes + 1u);
  if (client->outbound == NULL || client->inbound == NULL ||
      client->messages == NULL || client->received_data == NULL ||
      client->mqtt_buffer == NULL || client->output == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto failed;
  }
  memset(client->outbound, 0, pending_bytes);
  memset(client->inbound, 0, pending_bytes);
  *out_client = client;
  return H2_PAL_OK;
failed:
  (void)h2_onenet_destroy(&client);
  return rc;
}

h2_pal_result_t h2_onenet_destroy(h2_onenet_t** client) {
  if (client == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_onenet_t* value = *client;
  if (value == NULL) return H2_PAL_OK;
  if (value->busy || value->dispatching) return H2_PAL_ERR_BUSY;
  /* Partial create has no transport or initialized request tables. */
  if (value->mqtt_client != NULL)
    h2_pal_mqtt_close(value->config.mqtt, value->mqtt_client);
  h2_onenet_wipe(value->token, sizeof(value->token));
  if (value->mqtt_buffer != NULL)
    h2_onenet_wipe(value->mqtt_buffer, value->config.mqtt_buffer_bytes);
  const h2_pal_mem_api_t* mem = value->config.allocator;
  h2_pal_mem_free(mem, value->output);
  h2_pal_mem_free(mem, value->mqtt_buffer);
  h2_pal_mem_free(mem, value->received_data);
  h2_pal_mem_free(mem, value->messages);
  h2_pal_mem_free(mem, value->inbound);
  h2_pal_mem_free(mem, value->outbound);
  h2_pal_mem_free(mem, value);
  *client = NULL;
  return H2_PAL_OK;
}

h2_pal_result_t h2_onenet_connect(h2_onenet_t* client, const char* token) {
  if (client == NULL || token == NULL || token[0] == '\0')
    return H2_PAL_ERR_INVALID_ARG;
  if (client->busy || client->dispatching) return H2_PAL_ERR_BUSY;
  size_t token_len = 0u;
  while (token_len < sizeof(client->token) && token[token_len] != '\0')
    ++token_len;
  if (token_len == sizeof(client->token)) return H2_PAL_ERR_NO_SPACE;
  client->busy = true;
  close_transport(client, H2_PAL_ERR_CLOSED, true);
  memcpy(client->token, token, token_len + 1u);
  client->receive_error = H2_PAL_OK;
  client->transport_error = H2_PAL_OK;
  client->mqtt_config = (h2_pal_mqtt_client_config_t){
      .endpoint = client->config.endpoint,
      .client_id = {client->client_id, strlen(client->client_id)},
      .username = {client->product, strlen(client->product)},
      .password = {(const uint8_t*)client->token, token_len},
      .transport = client->config.transport,
      .tls = client->config.tls,
      .bind = client->config.bind,
      .keepalive_sec = client->config.keepalive_sec,
      .connect_timeout_ms = client->config.operation_timeout_ms,
      .operation_timeout_ms = client->config.operation_timeout_ms,
      .clean_session = 1,
      .network_buffer = client->mqtt_buffer,
      .network_buffer_len = client->config.mqtt_buffer_bytes,
      .on_event = on_mqtt_event,
      .event_user = client,
  };
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(
      client->config.time, &client->subscribe_started_ms);
  if (rc == H2_PAL_OK)
    rc = h2_pal_mqtt_open(client->config.mqtt, &client->mqtt_config,
                          &client->mqtt_client);
  if (rc == H2_PAL_OK)
    rc = h2_pal_mqtt_connect(client->config.mqtt, client->mqtt_client);
  if (rc == H2_PAL_OK)
    rc = h2_pal_time_get_monotonic_ms(client->config.time,
                                      &client->subscribe_started_ms);
  if (rc == H2_PAL_OK) {
    char filter[TOPIC_CAPACITY] = {0};
    h2_onenet_writer_t writer = {filter, sizeof(filter), 0u, H2_PAL_OK};
    h2_onenet_text(&writer, client->prefix);
    h2_onenet_text(&writer, "thing/#");
    const h2_pal_mqtt_subscribe_item_t item = {
        .filter = {filter, writer.length}, .qos = H2_PAL_MQTT_QOS1};
    const h2_pal_mqtt_subscribe_request_t request = {
        .items = &item,
        .item_count = 1u,
        .timeout_ms = client->config.operation_timeout_ms,
    };
    client->subscribing = true;
    rc = writer.result;
    if (rc == H2_PAL_OK)
      rc = h2_pal_mqtt_subscribe(client->config.mqtt, client->mqtt_client,
                                 &request, &client->subscribe_id);
  }
  if (rc == H2_PAL_OK && client->transport_error != H2_PAL_OK)
    rc = client->transport_error;
  if (rc != H2_PAL_OK) close_transport(client, rc, true);
  client->busy = false;
  return rc;
}

bool h2_onenet_is_connected(const h2_onenet_t* client) {
  return client != NULL && client->ready;
}

h2_pal_result_t h2_onenet_disconnect(h2_onenet_t* client) {
  if (client == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (client->busy || client->dispatching) return H2_PAL_ERR_BUSY;
  client->busy = true;
  h2_pal_result_t rc = H2_PAL_OK;
  if (client->mqtt_client != NULL && client->transport_error == H2_PAL_OK)
    rc = h2_pal_mqtt_disconnect(client->config.mqtt, client->mqtt_client,
                                client->config.operation_timeout_ms);
  close_transport(client, H2_PAL_ERR_CLOSED, true);
  client->busy = false;
  return rc;
}

static h2_pal_result_t parse_json(h2_onenet_t* client, const uint8_t* data,
                                  size_t len, bool wrapped,
                                  h2_pal_json_document_t** document,
                                  h2_pal_json_value_t** root) {
  *document = NULL;
  *root = NULL;
  if (data == NULL || len == 0u) return H2_PAL_ERR_FORMAT;
  const h2_pal_json_limits_t limits = {
      .max_document_bytes = client->config.limits.payload_bytes,
      .max_depth = client->config.limits.json_depth - (wrapped ? 1u : 0u),
      .max_values = client->config.limits.json_values - (wrapped ? 4u : 0u),
  };
  h2_pal_result_t rc = h2_pal_json_document_parse(client->config.json, data,
                                                  len, &limits, document);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_document_root(client->config.json, *document, root);
  return rc;
}

static bool value_type(h2_onenet_t* client, const h2_pal_json_value_t* value,
                       h2_pal_json_type_t expected) {
  h2_pal_json_type_t type = H2_PAL_JSON_TYPE_INVALID;
  return h2_pal_json_value_type(client->config.json, value, &type) ==
             H2_PAL_OK &&
         type == expected;
}

static h2_pal_result_t validate_params(h2_onenet_t* client,
                                       h2_onenet_operation_t operation,
                                       h2_pal_json_value_t* root) {
  if (operation != H2_ONENET_DESIRED_GET &&
      operation != H2_ONENET_DESIRED_DELETE &&
      operation != H2_ONENET_PROPERTY_GET)
    return value_type(client, root, H2_PAL_JSON_TYPE_OBJECT)
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
  if (!value_type(client, root, H2_PAL_JSON_TYPE_ARRAY))
    return H2_PAL_ERR_FORMAT;
  size_t count = 0u;
  h2_pal_result_t rc =
      h2_pal_json_array_size(client->config.json, root, &count);
  for (size_t i = 0u; rc == H2_PAL_OK && i < count; ++i) {
    h2_pal_json_value_t* item = NULL;
    h2_pal_json_string_view_t name = {0};
    rc = h2_pal_json_array_get(client->config.json, root, i, &item);
    if (rc == H2_PAL_OK)
      rc = h2_pal_json_value_get_string(client->config.json, item, &name);
    if (rc != H2_PAL_OK || name.len == 0u) return H2_PAL_ERR_FORMAT;
  }
  return rc;
}

static h2_pal_result_t publish_bytes(h2_onenet_t* client, const char* topic) {
  const h2_pal_mqtt_publish_t message = {
      .topic = {topic, strlen(topic)},
      .payload = {(const uint8_t*)client->output, strlen(client->output)},
      .qos = H2_PAL_MQTT_QOS0,
      .timeout_ms = client->config.operation_timeout_ms,
  };
  uint16_t packet_id = 0u;
  return h2_pal_mqtt_publish(client->config.mqtt, client->mqtt_client, &message,
                             &packet_id);
}

static h2_pal_result_t encode_message(h2_onenet_t* client, const char* id,
                                      bool response, int32_t code,
                                      const uint8_t* data, size_t len,
                                      h2_onenet_operation_t operation) {
  h2_pal_json_document_t* document = NULL;
  h2_pal_json_value_t* root = NULL;
  h2_pal_json_buffer_t buffer = {0};
  h2_pal_result_t rc = H2_PAL_OK;
  if (data != NULL || len != 0u) {
    rc = parse_json(client, data, len, true, &document, &root);
    if (rc == H2_PAL_OK)
      rc = response ? (value_type(client, root, H2_PAL_JSON_TYPE_OBJECT)
                           ? H2_PAL_OK
                           : H2_PAL_ERR_FORMAT)
                    : validate_params(client, operation, root);
    if (rc == H2_PAL_OK)
      rc = h2_pal_json_document_serialize(client->config.json, document,
                                          &buffer);
  } else if (!response) {
    rc = H2_PAL_ERR_INVALID_ARG;
  }
  if (rc == H2_PAL_OK) {
    client->output[0] = '\0';
    h2_onenet_writer_t writer = {client->output,
                                 client->config.limits.payload_bytes + 1u, 0u,
                                 H2_PAL_OK};
    h2_onenet_text(&writer, "{\"id\":\"");
    h2_onenet_text(&writer, id);
    if (response) {
      h2_onenet_text(&writer, "\",\"code\":");
      h2_onenet_decimal(&writer, (uint64_t)code);
      h2_onenet_text(&writer, ",\"msg\":\"\"");
      if (buffer.data != NULL) h2_onenet_text(&writer, ",\"data\":");
    } else {
      h2_onenet_text(&writer, "\",\"version\":\"1.0\",\"params\":");
    }
    if (buffer.data != NULL)
      h2_onenet_write(&writer, (const char*)buffer.data, buffer.len);
    h2_onenet_text(&writer, "}");
    rc = writer.result;
  }
  const h2_pal_result_t release_rc =
      h2_pal_json_buffer_release(client->config.json, &buffer);
  const h2_pal_result_t destroy_rc =
      h2_pal_json_document_destroy(client->config.json, &document);
  if (rc == H2_PAL_OK && release_rc != H2_PAL_OK) rc = release_rc;
  if (rc == H2_PAL_OK && destroy_rc != H2_PAL_OK) rc = destroy_rc;
  return rc;
}

static h2_pal_result_t make_topic(h2_onenet_t* client,
                                  h2_onenet_operation_t operation,
                                  bool response, const char* service,
                                  char* out) {
  out[0] = '\0';
  h2_onenet_writer_t writer = {out, TOPIC_CAPACITY, 0u, H2_PAL_OK};
  h2_onenet_text(&writer, client->prefix);
  if (operation == H2_ONENET_SERVICE_INVOKE) {
    h2_onenet_text(&writer, "thing/service/");
    h2_onenet_text(&writer, service);
    h2_onenet_text(&writer, response ? "/invoke_reply" : "/invoke");
    return writer.result;
  }
  for (size_t i = 0u; i < sizeof(topics) / sizeof(topics[0]); ++i)
    if (topics[i].operation == operation) {
      h2_onenet_text(&writer,
                     response ? topics[i].response : topics[i].request);
      return writer.result;
    }
  return H2_PAL_ERR_INVALID_ARG;
}

h2_pal_result_t h2_onenet_publish(h2_onenet_t* client,
                                  h2_onenet_operation_t operation,
                                  const uint8_t* params, size_t params_len,
                                  uint64_t* out_request_id) {
  if (out_request_id == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_request_id = 0u;
  if (client == NULL || params == NULL || params_len == 0u ||
      operation < H2_ONENET_PROPERTY_POST ||
      operation > H2_ONENET_DESIRED_DELETE)
    return H2_PAL_ERR_INVALID_ARG;
  if (client->busy || client->dispatching) return H2_PAL_ERR_BUSY;
  if (!client->ready) return H2_PAL_ERR_INVALID_STATE;
  pending_request_t* slot =
      empty_slot(client->outbound, client->config.limits.pending_requests);
  if (slot == NULL || client->next_id > H2_ONENET_MAX_REQUEST_ID)
    return H2_PAL_ERR_FULL;
  client->busy = true;
  memset(slot, 0, sizeof(*slot));
  h2_onenet_writer_t id = {slot->id, sizeof(slot->id), 0u, H2_PAL_OK};
  const uint64_t request_id = client->next_id++;
  h2_onenet_decimal(&id, request_id);
  h2_pal_result_t rc =
      encode_message(client, slot->id, false, 0, params, params_len, operation);
  char topic[TOPIC_CAPACITY];
  if (rc == H2_PAL_OK) rc = make_topic(client, operation, false, NULL, topic);
  if (rc == H2_PAL_OK)
    rc = h2_pal_time_get_monotonic_ms(client->config.time, &slot->started_ms);
  if (rc == H2_PAL_OK) rc = publish_bytes(client, topic);
  if (rc == H2_PAL_OK) {
    slot->operation = operation;
    slot->used = true;
    *out_request_id = request_id;
  }
  client->busy = false;
  return rc;
}

h2_pal_result_t h2_onenet_reply(h2_onenet_t* client, uint64_t command_handle,
                                int32_t code, const uint8_t* data,
                                size_t data_len) {
  if (client == NULL || command_handle == 0u || code < 0 ||
      (data == NULL && data_len != 0u) || (data != NULL && data_len == 0u))
    return H2_PAL_ERR_INVALID_ARG;
  if (client->busy && !client->dispatching) return H2_PAL_ERR_BUSY;
  if (!client->ready) return H2_PAL_ERR_INVALID_STATE;
  pending_request_t* slot = NULL;
  for (size_t i = 0u; i < client->config.limits.pending_requests; ++i)
    if (client->inbound[i].used &&
        client->inbound[i].handle == command_handle) {
      slot = &client->inbound[i];
      break;
    }
  if (slot == NULL) return H2_PAL_ERR_NOT_FOUND;
  uint64_t now = 0u;
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(client->config.time, &now);
  if (rc != H2_PAL_OK) return rc;
  if (deadline_expired(slot->started_ms, now,
                       client->config.response_timeout_ms))
    return H2_PAL_ERR_TIMEOUT;
  rc = encode_message(client, slot->id, true, code, data, data_len,
                      slot->operation);
  char topic[TOPIC_CAPACITY];
  if (rc == H2_PAL_OK)
    rc = make_topic(client, slot->operation, true, slot->service, topic);
  if (rc == H2_PAL_OK) rc = publish_bytes(client, topic);
  if (rc == H2_PAL_OK) slot->used = false;
  return rc;
}

static h2_pal_result_t object_get(h2_onenet_t* client,
                                  const h2_pal_json_value_t* object,
                                  const char* name,
                                  h2_pal_json_value_t** out_value) {
  return h2_pal_json_object_get(client->config.json, object, name, strlen(name),
                                out_value);
}

static h2_pal_result_t read_id(h2_onenet_t* client,
                               const h2_pal_json_value_t* root, char* out) {
  h2_pal_json_value_t* value = NULL;
  h2_pal_json_string_view_t id = {0};
  h2_pal_result_t rc = object_get(client, root, "id", &value);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_value_get_string(client->config.json, value, &id);
  if (rc != H2_PAL_OK || id.len == 0u ||
      id.len >= H2_ONENET_REQUEST_ID_CAPACITY)
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 0u; i < id.len; ++i)
    if (id.data[i] < '0' || id.data[i] > '9') return H2_PAL_ERR_FORMAT;
  memcpy(out, id.data, id.len);
  out[id.len] = '\0';
  return H2_PAL_OK;
}

static h2_pal_result_t dispatch_message(h2_onenet_t* client,
                                        const received_message_t* message,
                                        const uint8_t* payload) {
  h2_pal_json_document_t* document = NULL;
  h2_pal_json_value_t* root = NULL;
  h2_pal_result_t rc =
      parse_json(client, payload, message->length, false, &document, &root);
  char id[H2_ONENET_REQUEST_ID_CAPACITY] = {0};
  if (rc == H2_PAL_OK && !value_type(client, root, H2_PAL_JSON_TYPE_OBJECT))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) rc = read_id(client, root, id);
  h2_pal_json_value_t* version = NULL;
  if (rc == H2_PAL_OK && !message->response) {
    h2_pal_result_t version_rc = object_get(client, root, "version", &version);
    if (version_rc == H2_PAL_OK) {
      h2_pal_json_string_view_t text = {0};
      if (h2_pal_json_value_get_string(client->config.json, version, &text) !=
              H2_PAL_OK ||
          text.len != 3u || memcmp(text.data, "1.0", 3u) != 0)
        rc = H2_PAL_ERR_FORMAT;
    } else if (version_rc != H2_PAL_ERR_NOT_FOUND) {
      rc = version_rc;
    }
  }
  if (rc != H2_PAL_OK) goto done;
  h2_onenet_event_t event = {
      .type = message->response ? H2_ONENET_RESPONSE : H2_ONENET_COMMAND,
      .operation = message->operation,
      .result = H2_PAL_OK,
      .request_id = id,
      .service = message->service,
  };
  if (message->response) {
    h2_pal_json_value_t* value = NULL;
    double code = 0.0;
    if (object_get(client, root, "code", &value) != H2_PAL_OK ||
        h2_pal_json_value_get_number(client->config.json, value, &code) !=
            H2_PAL_OK ||
        code < 0.0 || code > (double)INT32_MAX ||
        code != (double)(int32_t)code) {
      rc = H2_PAL_ERR_FORMAT;
      goto done;
    }
    event.code = (int32_t)code;
    h2_pal_json_value_t* data = NULL;
    const h2_pal_result_t data_rc = object_get(client, root, "data", &data);
    if (data_rc != H2_PAL_OK && data_rc != H2_PAL_ERR_NOT_FOUND) {
      rc = data_rc;
      goto done;
    }
    event.data = data;
    for (size_t i = 0u; i < client->config.limits.pending_requests; ++i) {
      pending_request_t* slot = &client->outbound[i];
      if (slot->used && slot->operation == message->operation &&
          strcmp(slot->id, id) == 0) {
        slot->used = false;
        if (deadline_expired(slot->started_ms, message->received_ms,
                             client->config.response_timeout_ms))
          emit_result(client, H2_ONENET_RESPONSE, slot, H2_PAL_ERR_TIMEOUT);
        else
          emit(client, &event);
        break;
      }
    }
  } else {
    h2_pal_json_value_t* params = NULL;
    if (object_get(client, root, "params", &params) != H2_PAL_OK ||
        validate_params(client, message->operation, params) != H2_PAL_OK) {
      rc = H2_PAL_ERR_FORMAT;
      goto done;
    }
    event.data = params;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(client->config.time, &now);
    if (rc != H2_PAL_OK) goto done;
    if (deadline_expired(message->received_ms, now,
                         client->config.response_timeout_ms)) {
      rc = H2_PAL_ERR_TIMEOUT;
      goto done;
    }
    for (size_t i = 0u; i < client->config.limits.pending_requests; ++i) {
      const pending_request_t* slot = &client->inbound[i];
      if (slot->used && slot->operation == message->operation &&
          strcmp(slot->id, id) == 0 &&
          strcmp(slot->service, message->service) == 0)
        goto done;
    }
    pending_request_t* slot =
        empty_slot(client->inbound, client->config.limits.pending_requests);
    if (slot == NULL || client->next_handle == 0u) {
      rc = H2_PAL_ERR_FULL;
      goto done;
    }
    *slot = (pending_request_t){
        .used = true,
        .operation = message->operation,
        .handle = client->next_handle++,
        .started_ms = message->received_ms,
    };
    (void)copy_text(slot->id, sizeof(slot->id), id);
    (void)copy_text(slot->service, sizeof(slot->service), message->service);
    event.command_handle = slot->handle;
    emit(client, &event);
  }
done: {
  const h2_pal_result_t destroy_rc =
      h2_pal_json_document_destroy(client->config.json, &document);
  return rc != H2_PAL_OK ? rc : destroy_rc;
}
}

static void expire_requests(h2_onenet_t* client, uint64_t now) {
  for (size_t i = 0u; i < client->config.limits.pending_requests; ++i) {
    pending_request_t* outbound = &client->outbound[i];
    if (outbound->used &&
        deadline_expired(outbound->started_ms, now,
                         client->config.response_timeout_ms)) {
      outbound->used = false;
      emit_result(client, H2_ONENET_RESPONSE, outbound, H2_PAL_ERR_TIMEOUT);
    }
    pending_request_t* inbound = &client->inbound[i];
    if (inbound->used && deadline_expired(inbound->started_ms, now,
                                          client->config.response_timeout_ms)) {
      inbound->used = false;
      emit_result(client, H2_ONENET_COMMAND_EXPIRED, inbound,
                  H2_PAL_ERR_TIMEOUT);
    }
  }
}

h2_pal_result_t h2_onenet_process(h2_onenet_t* client, uint32_t timeout_ms) {
  if (client == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (client->busy || client->dispatching) return H2_PAL_ERR_BUSY;
  if (client->mqtt_client == NULL) return H2_PAL_ERR_INVALID_STATE;
  client->busy = true;
  h2_pal_result_t rc =
      h2_pal_mqtt_process(client->config.mqtt, client->mqtt_client, timeout_ms);
  if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) rc = H2_PAL_OK;
  if (rc == H2_PAL_OK) rc = client->transport_error;
  if (rc == H2_PAL_OK) rc = client->receive_error;
  uint64_t now = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_pal_time_get_monotonic_ms(client->config.time, &now);
  if (rc == H2_PAL_OK && client->subscribing &&
      deadline_expired(client->subscribe_started_ms, now,
                       client->config.operation_timeout_ms))
    rc = H2_PAL_ERR_TIMEOUT;
  if (rc != H2_PAL_OK) {
    emit_result(client, H2_ONENET_ERROR, NULL, rc);
    close_transport(client, rc, true);
    client->busy = false;
    return rc;
  }
  if (client->connected_event) {
    client->connected_event = false;
    emit_result(client, H2_ONENET_CONNECTED, NULL, H2_PAL_OK);
  }
  const size_t count = client->received_count;
  for (size_t i = 0u; i < count && client->transport_error == H2_PAL_OK &&
                      client->receive_error == H2_PAL_OK;
       ++i) {
    expire_requests(client, client->messages[i].received_ms);
    if (client->transport_error != H2_PAL_OK ||
        client->receive_error != H2_PAL_OK)
      break;
    const h2_pal_result_t message_rc = dispatch_message(
        client, &client->messages[i],
        client->received_data + i * client->config.limits.payload_bytes);
    if (message_rc != H2_PAL_OK) {
      emit_result(client, H2_ONENET_ERROR, NULL, message_rc);
      if (rc == H2_PAL_OK) rc = message_rc;
    }
  }
  /* A provider may deliver more input during a reply publish. Preserve those
   * copied messages for the next pump, rather than losing them at this
   * boundary. */
  client->received_count -= count;
  if (client->received_count != 0u) {
    memmove(client->messages, client->messages + count,
            client->received_count * sizeof(*client->messages));
    memmove(client->received_data,
            client->received_data + count * client->config.limits.payload_bytes,
            client->received_count * client->config.limits.payload_bytes);
  }
  const h2_pal_result_t time_rc =
      h2_pal_time_get_monotonic_ms(client->config.time, &now);
  if (time_rc == H2_PAL_OK)
    expire_requests(client, now);
  else {
    emit_result(client, H2_ONENET_ERROR, NULL, time_rc);
    close_transport(client, time_rc, true);
    if (rc == H2_PAL_OK) rc = time_rc;
  }
  /* Reply publishes and expiry callbacks may pump the provider too. Observe
   * their terminal errors before returning from this process call. */
  const h2_pal_result_t callback_rc = client->transport_error != H2_PAL_OK
                                          ? client->transport_error
                                          : client->receive_error;
  if (callback_rc != H2_PAL_OK && client->mqtt_client != NULL) {
    emit_result(client, H2_ONENET_ERROR, NULL, callback_rc);
    close_transport(client, callback_rc, true);
    rc = callback_rc;
  }
  client->busy = false;
  return rc;
}
